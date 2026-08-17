param(
    [string]$WorkRoot = (Join-Path $env:TEMP 'xfined-openssl-build-3.5.7'),
    [string]$InstallRoot = (Join-Path $env:TEMP 'xfined-openssl-install-3.5.7')
)

$ErrorActionPreference = 'Stop'
$opensslVersion = '3.5.7'
$opensslSha256 = 'A8C0D28A529CA480F9F36CF5792E2CD21984552A3C8E4AA11A24AA31AEAC98E8'
$strawberryVersion = '5.42.2.1'
$strawberrySha256 = '32D83BE90CF04B807CFB9477482BC36302CDEE6F5B04CF57E81ADECBD8F07898'
$nasmVersion = '3.02'
$nasmSha256 = '161D0BFAFF53C2F9E9F3E69FD0672323EBABAFD1268976A5CEC11BE92A19AEE7'

# Keep Strawberry Perl on its portable locale instead of inherited Unix values.
$env:LC_ALL = 'C'
$env:LC_CTYPE = 'C'
$env:LANG = 'C'

function Assert-EmptyDirectory([string]$Path)
{
    if (Test-Path -LiteralPath $Path)
    {
        if (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1)
        {
            throw "Directory must be empty: $Path"
        }
    }
    else
    {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Assert-Hash([string]$Path, [string]$Expected)
{
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    if ($actual -ne $Expected)
    {
        throw "SHA256 mismatch for $Path. Expected $Expected, got $actual."
    }
}

Assert-EmptyDirectory $WorkRoot
Assert-EmptyDirectory $InstallRoot

$opensslArchive = Join-Path $WorkRoot "openssl-$opensslVersion.tar.gz"
$strawberryArchive = Join-Path $WorkRoot "strawberry-perl-$strawberryVersion-64bit-portable.zip"
$nasmArchive = Join-Path $WorkRoot "nasm-$nasmVersion-win64.zip"
curl.exe -fL --retry 3 --output $opensslArchive "https://github.com/openssl/openssl/releases/download/openssl-$opensslVersion/openssl-$opensslVersion.tar.gz"
if ($LASTEXITCODE) { throw 'OpenSSL source download failed.' }
curl.exe -fL --retry 3 --output $strawberryArchive "https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54221_64bit/strawberry-perl-$strawberryVersion-64bit-portable.zip"
if ($LASTEXITCODE) { throw 'Strawberry Perl download failed.' }
curl.exe -fL --retry 3 --output $nasmArchive "https://www.nasm.us/pub/nasm/releasebuilds/$nasmVersion/win64/nasm-$nasmVersion-win64.zip"
if ($LASTEXITCODE) { throw 'NASM download failed.' }
Assert-Hash $opensslArchive $opensslSha256
Assert-Hash $strawberryArchive $strawberrySha256
Assert-Hash $nasmArchive $nasmSha256

$sourceParent = Join-Path $WorkRoot 'source'
$strawberryParent = Join-Path $WorkRoot 'strawberry'
$nasmParent = Join-Path $WorkRoot 'nasm'
$buildRoot = Join-Path $WorkRoot 'build'
New-Item -ItemType Directory -Path $sourceParent, $strawberryParent, $nasmParent, $buildRoot | Out-Null
tar.exe -xf $opensslArchive -C $sourceParent
if ($LASTEXITCODE) { throw 'OpenSSL source extraction failed.' }
Expand-Archive -LiteralPath $strawberryArchive -DestinationPath $strawberryParent
Expand-Archive -LiteralPath $nasmArchive -DestinationPath $nasmParent

$perl = Get-ChildItem -LiteralPath $strawberryParent -Recurse -Filter perl.exe -File |
    Where-Object { $_.FullName -match '[\\/]perl[\\/]bin[\\/]perl\.exe$' } |
    Select-Object -First 1
if (!$perl) { throw 'Perl executable is missing from the verified Strawberry archive.' }

$nasm = Get-ChildItem -LiteralPath $nasmParent -Recurse -Filter nasm.exe -File | Select-Object -First 1
if (!$nasm) { throw 'NASM executable is missing from the verified archive.' }
$env:PATH = "$($nasm.DirectoryName);$($perl.DirectoryName);$env:PATH"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio with x64 C++ tools was not found.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64' | Out-Null

$perlArchitecture = & $perl.FullName -V:archname
if ($perlArchitecture -notmatch 'MSWin32-x64')
{
    throw "Unexpected Perl architecture: $perlArchitecture"
}
Write-Host $perlArchitecture
& $nasm.FullName -v
& cl.exe 2>&1 | Select-Object -First 3

$sourceRoot = Join-Path $sourceParent "openssl-$opensslVersion"
Push-Location $buildRoot
try
{
    & $perl.FullName (Join-Path $sourceRoot 'Configure') 'VC-WIN64A' 'shared' 'no-makedepend' "--prefix=$InstallRoot" "--openssldir=$InstallRoot\ssl" '--libdir=lib'
    if ($LASTEXITCODE) { throw 'OpenSSL Configure failed.' }
    & nmake.exe
    if ($LASTEXITCODE) { throw 'OpenSSL build failed.' }
    & nmake.exe test
    if ($LASTEXITCODE) { throw 'OpenSSL tests failed.' }
    & nmake.exe install_sw
    if ($LASTEXITCODE) { throw 'OpenSSL install failed.' }
}
finally
{
    Pop-Location
}

$artifacts = @(
    (Join-Path $InstallRoot 'bin\libcrypto-3-x64.dll'),
    (Join-Path $InstallRoot 'lib\libcrypto.lib'),
    (Join-Path $InstallRoot 'include\openssl\configuration.h'),
    (Join-Path $InstallRoot 'include\openssl\opensslv.h')
)
foreach ($artifact in $artifacts)
{
    if (!(Test-Path -LiteralPath $artifact)) { throw "Expected artifact is missing: $artifact" }
}

& (Join-Path $InstallRoot 'bin\openssl.exe') version -a
Get-FileHash -Algorithm SHA256 -LiteralPath $artifacts | Format-Table -AutoSize
