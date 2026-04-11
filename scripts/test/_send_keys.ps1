# Send a key combo via user32!keybd_event.
#
# Args: one or more hex VK codes (e.g. 0x11 0x4C for Ctrl+L).
# Presses keys in argument order, waits briefly, then releases in reverse.
#
# Used by scripts/test/shell_launcher_smoke.sh.

param(
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$VkHex
)

Add-Type -Name SendKey -Namespace Test -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, uint dwExtraInfo);
'@

$codes = @()
foreach ($h in $VkHex) {
    $codes += [byte]([Convert]::ToInt32($h, 16))
}

# Press in forward order.
foreach ($c in $codes) {
    [Test.SendKey]::keybd_event($c, 0, 0, 0)
}
Start-Sleep -Milliseconds 60
# Release in reverse order.
for ($i = $codes.Length - 1; $i -ge 0; $i--) {
    [Test.SendKey]::keybd_event($codes[$i], 0, 2, 0)
}
