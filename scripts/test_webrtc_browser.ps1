param(
    [string]$VmHost = '36.50.27.142',
    [string]$VmKey = "$env:USERPROFILE\.ssh\dathuynh",
    [string]$Url = 'https://ai.protexa.cloud/',
    [int]$DevToolsPort = 9337,
    [string]$ScreenshotPath = ''
)

$ErrorActionPreference = 'Stop'
$credentialLines = ssh -i $VmKey -o BatchMode=yes "root@$VmHost" 'cat /root/jetsona-webrtc-credentials'
$username = (($credentialLines | Where-Object { $_ -like 'WEB_USER=*' }) -replace '^WEB_USER=', '')
$password = (($credentialLines | Where-Object { $_ -like 'WEB_PASSWORD=*' }) -replace '^WEB_PASSWORD=', '')
if (-not $username -or -not $password) { throw 'Unable to read WebRTC test credentials' }
$token = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${username}:${password}"))

$chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
$profile = Join-Path (Get-Location) ('.agents\chrome-e2e-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$process = Start-Process -FilePath $chrome -WindowStyle Hidden -PassThru -ArgumentList @(
    '--headless=new', '--no-first-run', '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required',
    "--remote-debugging-port=$DevToolsPort", "--user-data-dir=$profile", 'about:blank'
)

$script:socket = $null
$script:cdpId = 0

function Receive-CdpMessage {
    $buffer = New-Object byte[] 131072
    $stream = [IO.MemoryStream]::new()
    do {
        $segment = [ArraySegment[byte]]::new($buffer)
        $received = $script:socket.ReceiveAsync(
            $segment, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        $stream.Write($buffer, 0, $received.Count)
    } while (-not $received.EndOfMessage)
    [Text.Encoding]::UTF8.GetString($stream.ToArray())
}

function Invoke-Cdp([string]$Method, [hashtable]$Params = @{}) {
    $script:cdpId++
    $wanted = $script:cdpId
    $json = @{ id = $wanted; method = $Method; params = $Params } |
        ConvertTo-Json -Compress -Depth 10
    $data = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($data)
    $script:socket.SendAsync($segment, [Net.WebSockets.WebSocketMessageType]::Text,
        $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
    do {
        $message = (Receive-CdpMessage) | ConvertFrom-Json
    } while ($null -eq $message.id -or [int]$message.id -ne $wanted)
    $message
}

try {
    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        try {
            Invoke-RestMethod "http://127.0.0.1:$DevToolsPort/json/version" | Out-Null
            $ready = $true
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $ready) { throw 'Chrome DevTools did not start' }

    $target = Invoke-RestMethod -Method Put "http://127.0.0.1:$DevToolsPort/json/new"
    $script:socket = [Net.WebSockets.ClientWebSocket]::new()
    $script:socket.ConnectAsync([Uri]$target.webSocketDebuggerUrl,
        [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
    Invoke-Cdp 'Network.enable' | Out-Null
    Invoke-Cdp 'Network.setExtraHTTPHeaders' @{ headers = @{ Authorization = "Basic $token" } } | Out-Null
    Invoke-Cdp 'Page.enable' | Out-Null
    Invoke-Cdp 'Page.navigate' @{ url = $Url } | Out-Null
    Start-Sleep -Seconds 12
    $expression = @"
JSON.stringify({
  status: document.querySelector('#status') && document.querySelector('#status').textContent,
  ready: document.querySelector('video') && document.querySelector('video').readyState,
  width: document.querySelector('video') && document.querySelector('video').videoWidth,
  height: document.querySelector('video') && document.querySelector('video').videoHeight
})
"@
    $evaluation = Invoke-Cdp 'Runtime.evaluate' @{
        expression = $expression
        returnByValue = $true
    }
    $evaluation.result.result.value
    if ($ScreenshotPath) {
        $capture = Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png'; fromSurface = $true }
        [IO.File]::WriteAllBytes(
            [IO.Path]::GetFullPath($ScreenshotPath),
            [Convert]::FromBase64String($capture.result.data))
    }
    Invoke-Cdp 'Browser.close' | Out-Null
} finally {
    if ($script:socket) { $script:socket.Dispose() }
    if ($process -and -not $process.HasExited) {
        $process.WaitForExit(3000) | Out-Null
        if (-not $process.HasExited) { Stop-Process -Id $process.Id }
    }
}
