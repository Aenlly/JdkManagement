#include "providers/download_helpers.h"

#include <iostream>

namespace jkm {

std::string BuildDownloadHelperScript() {
    return std::string(R"PS(function Write-JkmLog {
  param([string]$Message)
  Write-Output ('LOG=' + $Message)
}
function Format-JkmSize {
  param([Int64]$Bytes)
  if ($Bytes -lt 1MB) { return ('{0} KB' -f [Math]::Max(1, [Math]::Round($Bytes / 1KB))) }
  if ($Bytes -lt 1GB) { return ('{0:0.0} MB' -f ($Bytes / 1MB)) }
  return ('{0:0.00} GB' -f ($Bytes / 1GB))
}
function Format-JkmRate {
  param([double]$BytesPerSecond)
  if ($BytesPerSecond -le 0) { return 'n/a' }
  return ((Format-JkmSize ([Int64][Math]::Round($BytesPerSecond))) + '/s')
}
function Format-JkmDuration {
  param([double]$Seconds)
  if ($Seconds -lt 0) { $Seconds = 0 }
  $totalSeconds = [int][Math]::Ceiling($Seconds)
  $hours = [int]($totalSeconds / 3600)
  $minutes = [int](($totalSeconds % 3600) / 60)
  $secs = [int]($totalSeconds % 60)
  if ($hours -gt 0) { return ('{0:D2}:{1:D2}:{2:D2}' -f $hours, $minutes, $secs) }
  return ('{0:D2}:{1:D2}' -f $minutes, $secs)
}
$script:JkmNetworkInitialized = $false
$script:JkmTrustedRootCertificates = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection
function Get-JkmEnvironmentValue {
  param([string[]]$Names)
  foreach ($name in $Names) {
    if ([string]::IsNullOrWhiteSpace($name)) { continue }
    $value = [System.Environment]::GetEnvironmentVariable($name, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($value)) { return $value.Trim() }
  }
  return ''
}
function Get-JkmSourceBaseUrl {
  param([string]$EnvironmentVariableName, [string]$DefaultBaseUrl)
  $configured = Get-JkmEnvironmentValue @($EnvironmentVariableName)
  if ([string]::IsNullOrWhiteSpace($configured)) { return $DefaultBaseUrl.TrimEnd('/') }
  return $configured.Trim().TrimEnd('/')
}
function Join-JkmUri {
  param([string]$BaseUrl, [string]$RelativePath)
  if ([string]::IsNullOrWhiteSpace($RelativePath)) { return $BaseUrl }
  if ($RelativePath -match '^[a-zA-Z][a-zA-Z0-9+.-]*://') { return $RelativePath }
  return ($BaseUrl.TrimEnd('/') + '/' + $RelativePath.TrimStart('/'))
}
function Get-JkmCertificateCollection {
  param([string]$Path)
  $certificates = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection
  if (-not (Test-Path $Path)) { return $certificates }
  $content = Get-Content -Raw -Path $Path
  $matches = [regex]::Matches($content, '-----BEGIN CERTIFICATE-----(?<body>.*?)-----END CERTIFICATE-----', [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if ($matches.Count -gt 0) {
    foreach ($match in $matches) {
      $base64 = ($match.Groups['body'].Value -replace '\s', '')
      if ([string]::IsNullOrWhiteSpace($base64)) { continue }
      $bytes = [System.Convert]::FromBase64String($base64)
      [void]$certificates.Add([System.Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes))
    }
    return $certificates
  }
  $certificates.Import($Path)
  return $certificates
}
function Initialize-JkmNetwork {
  if ($script:JkmNetworkInitialized) { return }
  $httpProxy = Get-JkmEnvironmentValue @('JDKM_HTTP_PROXY', 'HTTP_PROXY', 'http_proxy')
  $httpsProxy = Get-JkmEnvironmentValue @('JDKM_HTTPS_PROXY', 'HTTPS_PROXY', 'https_proxy')
  $proxyUrl = if (-not [string]::IsNullOrWhiteSpace($httpsProxy)) { $httpsProxy } else { $httpProxy }
  if (-not [string]::IsNullOrWhiteSpace($proxyUrl)) {
    $proxy = [System.Net.WebProxy]::new($proxyUrl, $true)
    $proxy.Credentials = [System.Net.CredentialCache]::DefaultCredentials
    [System.Net.WebRequest]::DefaultWebProxy = $proxy
  }
  $caPath = Get-JkmEnvironmentValue @('JDKM_CA_CERT_PATH', 'SSL_CERT_FILE')
  if (-not [string]::IsNullOrWhiteSpace($caPath) -and (Test-Path $caPath)) {
    $script:JkmTrustedRootCertificates = Get-JkmCertificateCollection -Path $caPath
    if ($script:JkmTrustedRootCertificates.Count -gt 0) {
      [System.Net.ServicePointManager]::ServerCertificateValidationCallback = {
        param($sender, $certificate, $chain, $sslPolicyErrors)
        if ($sslPolicyErrors -eq [System.Net.Security.SslPolicyErrors]::None) { return $true }
        if (-not $certificate) { return $false }
        $serverCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $certificate
        $customChain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
        $customChain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
        $customChain.ChainPolicy.VerificationFlags = [System.Security.Cryptography.X509Certificates.X509VerificationFlags]::AllowUnknownCertificateAuthority
        foreach ($trusted in $script:JkmTrustedRootCertificates) {
          [void]$customChain.ChainPolicy.ExtraStore.Add($trusted)
        }
        if (-not $customChain.Build($serverCertificate)) { return $false }
        $root = $customChain.ChainElements[$customChain.ChainElements.Count - 1].Certificate
        foreach ($trusted in $script:JkmTrustedRootCertificates) {
          if ($root.Thumbprint -eq $trusted.Thumbprint) { return $true }
        }
        return $false
      }
    }
  }
  $script:JkmNetworkInitialized = $true
}
function Invoke-JkmRestMethod {
  param([string]$Uri)
  Initialize-JkmNetwork
  return Invoke-RestMethod -Uri $Uri -Headers @{ 'User-Agent' = 'jkm-cpp' }
}
function Invoke-JkmWebRequestContent {
  param([string]$Uri)
  Initialize-JkmNetwork
  return (Invoke-WebRequest -Uri $Uri -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content
}
function Get-JkmPartialDownloadPath {
  param([string]$Destination)
  return ($Destination + '.partial')
}
function New-JkmWebRequest {
  param([string]$Uri, [Int64]$ResumeFrom)
  Initialize-JkmNetwork
  $request = [System.Net.HttpWebRequest][System.Net.WebRequest]::Create($Uri)
  $request.UserAgent = 'jkm-cpp'
  $request.AutomaticDecompression = [System.Net.DecompressionMethods]::GZip -bor [System.Net.DecompressionMethods]::Deflate
  $request.AllowAutoRedirect = $true
  $request.Timeout = 30000
  $request.ReadWriteTimeout = 30000
  if ($ResumeFrom -gt 0) { $request.AddRange($ResumeFrom) }
  return $request
}
function Get-JkmDownloadTotalBytes {
  param([System.Net.HttpWebResponse]$Response, [Int64]$ResumeFrom)
  $contentRange = [string]$Response.Headers['Content-Range']
  if (-not [string]::IsNullOrWhiteSpace($contentRange) -and $contentRange -match '/(?<total>\d+)$') {
    return [Int64]$Matches['total']
  }
  $contentLength = [Int64]$Response.ContentLength
  if ($contentLength -le 0) { return [Int64]-1 }
  if ($ResumeFrom -gt 0 -and $Response.StatusCode -eq [System.Net.HttpStatusCode]::PartialContent) {
    return [Int64]($ResumeFrom + $contentLength)
  }
  return $contentLength
}
function Get-JkmRetryDelaySeconds {
  param([int]$AttemptNumber)
  switch ($AttemptNumber) {
    1 { return 2 }
    2 { return 4 }
    default { return 8 }
  }
}
function Get-JkmDownloadErrorMessage {
  param([System.Exception]$Exception)
  if ($Exception -is [System.Net.WebException]) {
    $webException = [System.Net.WebException]$Exception
    if ($webException.Response -and $webException.Response -is [System.Net.HttpWebResponse]) {
      $response = [System.Net.HttpWebResponse]$webException.Response
      return ('HTTP ' + [int]$response.StatusCode + ' ' + $response.StatusDescription)
    }
  }
  $message = [string]$Exception.Message
  if ([string]::IsNullOrWhiteSpace($message)) {
    $message = ($Exception | Out-String).Trim()
  }
  return $message.Trim()
}
)PS") + R"PS(
function Get-JkmTransferRateSnapshot {
  param(
    [Int64]$DownloadedBytes,
    [Int64]$TotalBytes,
    [System.DateTime]$StartedAt,
    [Int64]$RateStartBytes,
    [Int64]$WindowStartBytes,
    [System.DateTime]$WindowStartedAt,
    [double]$PreviousSmoothedBytesPerSecond,
    [System.DateTime]$Now
  )
  $totalElapsedSeconds = [Math]::Max(0.001, ($Now - $StartedAt).TotalSeconds)
  $windowElapsedSeconds = [Math]::Max(0.001, ($Now - $WindowStartedAt).TotalSeconds)
  $sessionDownloadedBytes = [double][Math]::Max(0, $DownloadedBytes - $RateStartBytes)
  $overallBytesPerSecond = $sessionDownloadedBytes / [Math]::Max($totalElapsedSeconds, 3.0)
  $windowBytes = [double][Math]::Max(0, $DownloadedBytes - $WindowStartBytes)
  $windowBytesPerSecond = if ($windowBytes -gt 0) { $windowBytes / [Math]::Max($windowElapsedSeconds, 3.0) } else { 0.0 }
  $progressRatio = if ($TotalBytes -gt 0) { [Math]::Min(1.0, [double]$DownloadedBytes / [double]$TotalBytes) } else { 0.0 }
  if ($PreviousSmoothedBytesPerSecond -le 0) {
    $smoothedBytesPerSecond = if ($windowBytesPerSecond -gt 0) { $windowBytesPerSecond } else { $overallBytesPerSecond }
  } elseif ($windowBytesPerSecond -le 0) {
    $smoothedBytesPerSecond = $PreviousSmoothedBytesPerSecond
  } else {
    $alpha = if ($totalElapsedSeconds -lt 12 -or $progressRatio -lt 0.25) { 0.18 } elseif ($totalElapsedSeconds -lt 30) { 0.24 } else { 0.32 }
    $smoothedBytesPerSecond = (($PreviousSmoothedBytesPerSecond * (1.0 - $alpha)) + ($windowBytesPerSecond * $alpha))
  }
  $warmupWeight = [Math]::Min(1.0, [Math]::Max(0.0, ($totalElapsedSeconds - 4.0) / 10.0))
  $displayBytesPerSecond = (($overallBytesPerSecond * (1.0 - $warmupWeight)) + ($smoothedBytesPerSecond * $warmupWeight))
  return @{
    SmoothedBytesPerSecond = $smoothedBytesPerSecond
    DisplayBytesPerSecond = $displayBytesPerSecond
  }
}
function Get-JkmSmoothedRemainingSeconds {
  param(
    [double]$RawRemainingSeconds,
    [double]$PreviousRemainingSeconds,
    [System.DateTime]$StartedAt,
    [Int64]$DownloadedBytes,
    [Int64]$TotalBytes,
    [System.DateTime]$Now
  )
  if ($RawRemainingSeconds -lt 0) { return -1.0 }
  if ($PreviousRemainingSeconds -lt 0) { return $RawRemainingSeconds }
  if ($RawRemainingSeconds -le 0.5) { return 0.0 }
  $elapsedSeconds = [Math]::Max(0.001, ($Now - $StartedAt).TotalSeconds)
  $progressRatio = if ($TotalBytes -gt 0) { [Math]::Min(1.0, [double]$DownloadedBytes / [double]$TotalBytes) } else { 0.0 }
  $isImproving = $RawRemainingSeconds -lt $PreviousRemainingSeconds
  if ($isImproving) {
    $alpha = if ($progressRatio -lt 0.25) { 0.45 } elseif ($progressRatio -lt 0.60) { 0.60 } else { 0.72 }
    if ($RawRemainingSeconds -lt ($PreviousRemainingSeconds * 0.5)) { $alpha = [Math]::Max($alpha, 0.80) }
  } else {
    $alpha = if ($elapsedSeconds -lt 12 -or $progressRatio -lt 0.25) { 0.10 } elseif ($elapsedSeconds -lt 30) { 0.14 } else { 0.20 }
  }
  $smoothedRemainingSeconds = (($PreviousRemainingSeconds * (1.0 - $alpha)) + ($RawRemainingSeconds * $alpha))
  if ($isImproving) { return $smoothedRemainingSeconds }
  $maxIncreaseSeconds = if ($progressRatio -lt 0.15) { 15.0 } elseif ($progressRatio -lt 0.35) { 12.0 } else { 20.0 }
  return [Math]::Min($smoothedRemainingSeconds, $PreviousRemainingSeconds + $maxIncreaseSeconds)
}
function Should-JkmShowEta {
  param(
    [Int64]$DownloadedBytes,
    [Int64]$TotalBytes,
    [System.DateTime]$StartedAt,
    [System.DateTime]$Now
  )
  if ($TotalBytes -le 0) { return $false }
  $elapsedSeconds = ($Now - $StartedAt).TotalSeconds
  $progressRatio = [double]$DownloadedBytes / [double][Math]::Max(1, $TotalBytes)
  return ($DownloadedBytes -ge 2MB) -or ($elapsedSeconds -ge 8) -or ($progressRatio -ge 0.10)
}
)PS" + R"PS(
function Download-JkmFile {
  param([string]$Uri, [string]$Destination, [string]$Label)
  $parent = Split-Path -Parent $Destination
  if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
  $partialPath = Get-JkmPartialDownloadPath $Destination
  $buffer = New-Object byte[] 65536
  $rateText = 'n/a'
  $lastReportedPercent = -1
  $smoothedBytesPerSecond = [double]0
  $smoothedRemainingSeconds = [double]-1
  $downloadStartedAt = [System.DateTime]::UtcNow
  $lastLoggedAt = $downloadStartedAt
  $windowStartedAt = $downloadStartedAt
  $windowStartBytes = [Int64]0
  $activeTransferSeconds = [double]0
  $rateStartBytes = if (Test-Path $partialPath) { [Int64](Get-Item $partialPath).Length } else { [Int64]0 }
  $downloadedBytes = $rateStartBytes
  $totalBytes = [Int64]-1
  $didLogTotalBytes = $false
  if ($downloadedBytes -gt 0) {
    Write-JkmLog ('Resuming partial download (' + $Label + '): ' + (Format-JkmSize $downloadedBytes) + ' already downloaded')
    $windowStartBytes = $downloadedBytes
  }
  $completed = $false
  $maxAttempts = 3
  for ($attempt = 1; $attempt -le $maxAttempts -and -not $completed; $attempt++) {
    $request = $null
    $response = $null
    $responseStream = $null
    $fileStream = $null
    $attemptStartedAt = [System.DateTime]::UtcNow
    $resumeFrom = if (Test-Path $partialPath) { [Int64](Get-Item $partialPath).Length } else { [Int64]0 }
    $downloadedBytes = $resumeFrom
    if ($resumeFrom -gt 0) {
      $windowStartedAt = $attemptStartedAt
      $windowStartBytes = $downloadedBytes
      $lastLoggedAt = $attemptStartedAt
    }
    try {
      $request = New-JkmWebRequest -Uri $Uri -ResumeFrom $resumeFrom
      $response = [System.Net.HttpWebResponse]$request.GetResponse()
      if ($resumeFrom -gt 0 -and $response.StatusCode -ne [System.Net.HttpStatusCode]::PartialContent) {
        Write-JkmLog ('Download source restarted transfer (' + $Label + '); discarding ' + (Format-JkmSize $resumeFrom) + ' partial data')
        if (Test-Path $partialPath) { Remove-Item -Force $partialPath }
        $downloadedBytes = 0
        $rateStartBytes = 0
        $lastReportedPercent = -1
        $smoothedBytesPerSecond = [double]0
        $smoothedRemainingSeconds = [double]-1
        $windowStartedAt = [System.DateTime]::UtcNow
        $windowStartBytes = [Int64]0
        $lastLoggedAt = $windowStartedAt
        $attempt--
        continue
      }
      $attemptTotalBytes = Get-JkmDownloadTotalBytes -Response $response -ResumeFrom $resumeFrom
      if ($attemptTotalBytes -gt 0) {
        $totalBytes = $attemptTotalBytes
        if (-not $didLogTotalBytes) {
          Write-JkmLog ('Download size (' + $Label + '): ' + (Format-JkmSize $totalBytes))
          $didLogTotalBytes = $true
        }
      }
      if ($resumeFrom -gt 0 -and $totalBytes -gt 0) {
        $lastReportedPercent = [Math]::Max($lastReportedPercent, [int][Math]::Floor(($downloadedBytes * 100) / $totalBytes))
      }
      $responseStream = $response.GetResponseStream()
      $fileMode = if ($resumeFrom -gt 0) { [System.IO.FileMode]::Append } else { [System.IO.FileMode]::Create }
      $fileStream = [System.IO.File]::Open($partialPath, $fileMode, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
      while (($read = $responseStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $fileStream.Write($buffer, 0, $read)
        $downloadedBytes += $read
        $now = [System.DateTime]::UtcNow
        $elapsedSinceLastLogSeconds = ($now - $lastLoggedAt).TotalSeconds
        $shouldLog = $false
        if ($totalBytes -gt 0) {
          $percent = [int][Math]::Floor(($downloadedBytes * 100) / $totalBytes)
          if ($percent -ge 100 -or $percent -ge ($lastReportedPercent + 5) -or $elapsedSinceLastLogSeconds -ge 1) {
            $shouldLog = $true
          }
        } elseif ($elapsedSinceLastLogSeconds -ge 1) {
          $shouldLog = $true
        }
        if (-not $shouldLog) { continue }
        $effectiveStartedAt = $attemptStartedAt.AddSeconds(-$activeTransferSeconds)
        $rateState = Get-JkmTransferRateSnapshot -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -StartedAt $effectiveStartedAt -RateStartBytes $rateStartBytes -WindowStartBytes $windowStartBytes -WindowStartedAt $windowStartedAt -PreviousSmoothedBytesPerSecond $smoothedBytesPerSecond -Now $now
        $smoothedBytesPerSecond = [double]$rateState.SmoothedBytesPerSecond
        $displayBytesPerSecond = [double]$rateState.DisplayBytesPerSecond
        $rateText = Format-JkmRate $displayBytesPerSecond
        if ($totalBytes -gt 0) {
          $showEta = Should-JkmShowEta -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -StartedAt $effectiveStartedAt -Now $now
          $remainingBytes = [Math]::Max(0, $totalBytes - $downloadedBytes)
          $rawRemainingSeconds = if ($displayBytesPerSecond -gt 0) { $remainingBytes / $displayBytesPerSecond } else { -1 }
          if ($remainingBytes -le 0 -or $percent -ge 100) {
            $smoothedRemainingSeconds = 0.0
          } elseif ($showEta) {
            $smoothedRemainingSeconds = Get-JkmSmoothedRemainingSeconds -RawRemainingSeconds $rawRemainingSeconds -PreviousRemainingSeconds $smoothedRemainingSeconds -StartedAt $effectiveStartedAt -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -Now $now
          }
          if ($showEta -or $percent -ge 100) {
            $etaText = if ($smoothedRemainingSeconds -ge 0) { Format-JkmDuration $smoothedRemainingSeconds } else { 'n/a' }
            Write-JkmLog ('Download progress (' + $Label + '): ' + $percent + '% (' + (Format-JkmSize $downloadedBytes) + ' / ' + (Format-JkmSize $totalBytes) + ', ' + $rateText + ', ETA ' + $etaText + ')')
          } else {
            Write-JkmLog ('Download progress (' + $Label + '): ' + $percent + '% (' + (Format-JkmSize $downloadedBytes) + ' / ' + (Format-JkmSize $totalBytes) + ', ' + $rateText + ')')
          }
          $lastReportedPercent = $percent
        } else {
          Write-JkmLog ('Download progress (' + $Label + '): ' + (Format-JkmSize $downloadedBytes) + ' at ' + $rateText)
        }
        $lastLoggedAt = $now
        if (($now - $windowStartedAt).TotalSeconds -ge 3 -and ($downloadedBytes - $windowStartBytes) -ge 1MB) {
          $windowStartedAt = $now
          $windowStartBytes = $downloadedBytes
        }
      }
      $activeTransferSeconds += [Math]::Max(0.0, ([System.DateTime]::UtcNow - $attemptStartedAt).TotalSeconds)
      $completed = $true
    } catch {
      $activeTransferSeconds += [Math]::Max(0.0, ([System.DateTime]::UtcNow - $attemptStartedAt).TotalSeconds)
      $message = Get-JkmDownloadErrorMessage $_.Exception
      if ($attempt -ge $maxAttempts) {
        throw ('download failed after ' + $attempt + ' attempts: ' + $message)
      }
      $resumeBytes = if (Test-Path $partialPath) { [Int64](Get-Item $partialPath).Length } else { [Int64]0 }
      $downloadedBytes = $resumeBytes
      $delaySeconds = Get-JkmRetryDelaySeconds -AttemptNumber $attempt
      $resumeText = if ($resumeBytes -gt 0) { ' from ' + (Format-JkmSize $resumeBytes) } else { '' }
      Write-JkmLog ('Download attempt ' + $attempt + ' failed (' + $Label + '): ' + $message + '; retrying in ' + $delaySeconds + 's' + $resumeText)
      $resetAt = [System.DateTime]::UtcNow
      $lastLoggedAt = $resetAt
      $windowStartedAt = $resetAt
      $windowStartBytes = $downloadedBytes
      Start-Sleep -Seconds $delaySeconds
    } finally {
      if ($fileStream) { $fileStream.Dispose() }
      if ($responseStream) { $responseStream.Dispose() }
      if ($response) { $response.Dispose() }
    }
  }
  if (-not $completed) {
    throw 'download did not complete'
  }
  if (Test-Path $Destination) { Remove-Item -Force $Destination }
  if (Test-Path $partialPath) { Move-Item -Force $partialPath $Destination }
  $finishedAt = [System.DateTime]::UtcNow
  if ($downloadedBytes -gt 0) {
    $effectiveStartedAt = $finishedAt.AddSeconds(-$activeTransferSeconds)
    $rateState = Get-JkmTransferRateSnapshot -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -StartedAt $effectiveStartedAt -RateStartBytes $rateStartBytes -WindowStartBytes $windowStartBytes -WindowStartedAt $windowStartedAt -PreviousSmoothedBytesPerSecond $smoothedBytesPerSecond -Now $finishedAt
    $rateText = Format-JkmRate ([double]$rateState.DisplayBytesPerSecond)
  }
  if ($totalBytes -le 0) {
    Write-JkmLog ('Download completed (' + $Label + '): ' + (Format-JkmSize $downloadedBytes) + ' at ' + $rateText)
  } elseif ($lastReportedPercent -lt 100) {
    Write-JkmLog ('Download progress (' + $Label + '): 100% (' + (Format-JkmSize $downloadedBytes) + ' / ' + (Format-JkmSize $totalBytes) + ', ' + $rateText + ', ETA 00:00)')
  }
}
)PS";
}

ProcessOutputLineHandler BuildDownloadOutputLineHandler() {
    return [](const std::string& line) {
        if (line.rfind("LOG=", 0) == 0) {
            std::cout << line.substr(4) << std::endl;
        }
    };
}

}  // namespace jkm
