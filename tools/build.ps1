$ErrorActionPreference = "Stop"

$root = "C:\Users\LOQ\esp8266-stm32-probe"
$ref  = Get-Content -LiteralPath "$root\esp32_main_reference.cpp"

# Lines 99..1501 (1-based) -> indices 98..1500. This is the portable core:
# SWD transport, AP layer, Cortex-M control, STM32F1 flash, native protocol.
$mid = ($ref[98..1500] -join "`n")

$repl = @(
  @{ old = "    for (uint32_t i = 0; i < nwords; i++) {`n        st = swd_read_reg(false, 0x0c, &buf[i], false); /* DP RDBUFF */";
     new = "    for (uint32_t i = 0; i < nwords; i++) {`n        if ((i & 0x3F) == 0) yield();`n        st = swd_read_reg(false, 0x0c, &buf[i], false); /* DP RDBUFF */" },
  @{ old = "    for (uint32_t i = 0; i < nwords; i++) {`n        uint32_t cur_addr = addr + i * 4;";
     new = "    for (uint32_t i = 0; i < nwords; i++) {`n        if ((i & 0x3F) == 0) yield();`n        uint32_t cur_addr = addr + i * 4;" },
  @{ old = "        for (uint32_t off = 0; off + 1 < len; off += 2) {`n            uint32_t a = addr + off;";
     new = "        for (uint32_t off = 0; off + 1 < len; off += 2) {`n            if ((off & 0xFF) == 0) yield();`n            uint32_t a = addr + off;" },
  @{ old = "    for (int i = 0; i < max_steps; i++) {`n        int st = cortex_step_write();           /* single step, interrupts masked */";
     new = "    for (int i = 0; i < max_steps; i++) {`n        if ((i & 0x3F) == 0) yield();`n        int st = cortex_step_write();           /* single step, interrupts masked */" },
  @{ old = "    for (int retry = 0; retry < 1000; retry++) {`n        uint8_t cmd = SWD_RNW | (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);";
     new = "    for (int retry = 0; retry < 1000; retry++) {`n        if ((retry & 0x3F) == 0) yield();`n        uint8_t cmd = SWD_RNW | (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);" },
  @{ old = "    for (int retry = 0; retry < 500; retry++) {`n        uint8_t cmd = (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);";
     new = "    for (int retry = 0; retry < 500; retry++) {`n        if ((retry & 0x3F) == 0) yield();`n        uint8_t cmd = (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);" }
)

foreach ($r in $repl) {
  if ($mid.Contains($r.old)) {
    $mid = $mid.Replace($r.old, $r.new)
    Write-Host "OK   inserted yield"
  } else {
    Write-Host "MISS yield anchor not found!" -ForegroundColor Red
  }
}

$header = Get-Content -Raw -LiteralPath "$root\build_tmp\header.txt"
$footer = Get-Content -Raw -LiteralPath "$root\build_tmp\footer.txt"

$out = $header + "`n" + $mid + "`n" + $footer

$dest = "$root\src\ESP8266_STM32_Probe"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Set-Content -LiteralPath "$dest\ESP8266_STM32_Probe.ino" -Value $out -Encoding UTF8

Write-Host ("Wrote {0} bytes to {1}\ESP8266_STM32_Probe.ino" -f $out.Length, $dest)
