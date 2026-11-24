# Installs Cato OS onto the dedicated USB drive. Run from Windows PowerShell or pwsh.
# It adds or replaces only \EFI\BOOT\BOOTX64.EFI and the \CATOOS\STORE.ID marker.
# Saved files are never changed, and the drive is never formatted.
param(
    [string]$DriveLetter = 'E',
    [string]$EfiPath = (Join-Path $PSScriptRoot '..\build\boot\BOOTX64.EFI')
)
$ErrorActionPreference = 'Stop'

function Require([bool]$condition, [string]$what) {
    if (-not $condition) { throw "Refusing to install: $what" }
}

Require (Test-Path -LiteralPath $EfiPath) "the boot file was not found at $EfiPath. Run make first."

$volume = Get-Volume -DriveLetter $DriveLetter
$partition = Get-Partition -DriveLetter $DriveLetter
$disk = Get-Disk -Number $partition.DiskNumber

Require ($disk.BusType -eq 'USB') "${DriveLetter}: is not a USB drive"
Require (-not $disk.IsBoot -and -not $disk.IsSystem) "${DriveLetter}: is a boot or system disk"
Require ("$($volume.DriveType)" -eq 'Removable') "${DriveLetter}: is not removable"
Require ($volume.FileSystem -eq 'FAT32') "${DriveLetter}: is not FAT32"
Require ($volume.FileSystemLabel -eq 'CATO_OS') "${DriveLetter}: is not labeled CATO_OS"

$root = "${DriveLetter}:\"
$bootDirectory = Join-Path $root 'EFI\BOOT'
$storeDirectory = Join-Path $root 'CATOOS'
$null = New-Item -ItemType Directory -Force -Path $bootDirectory
$null = New-Item -ItemType Directory -Force -Path $storeDirectory

$marker = Join-Path $storeDirectory 'STORE.ID'
if (-not (Test-Path -LiteralPath $marker)) {
    $text = "CATO-STORE-V1`n$([guid]::NewGuid().ToString('N'))`n"
    [System.IO.File]::WriteAllText($marker, $text, [System.Text.Encoding]::ASCII)
}

$target = Join-Path $bootDirectory 'BOOTX64.EFI'
$temporary = "$target.new"
Copy-Item -LiteralPath $EfiPath -Destination $temporary -Force
if ((Get-FileHash -LiteralPath $EfiPath).Hash -ne (Get-FileHash -LiteralPath $temporary).Hash) {
    Remove-Item -LiteralPath $temporary -Force
    throw 'The copied boot file did not verify. The existing install was not changed.'
}
Move-Item -LiteralPath $temporary -Destination $target -Force
Write-Output "Installed Cato OS to ${DriveLetter}:"
