#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for scripts/lib/Ed25519.ps1.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    A signature check that says yes to everything is worse than no signature at all,
    because the gate that reads it reports a proof it never had. So the arithmetic is
    pinned to RFC 8032's own test vectors -- known seed, known public key, known
    message, known signature -- and every way a bad signature could be waved through
    has a case: a flipped message byte, a flipped signature byte, the right signature
    under the wrong key, a truncated or non-hex signature, and an s value at or above
    the group order, which encodes the same point as a smaller one and would otherwise
    make a signature malleable.

    Nothing here touches the network, a real key, or a real release.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $scriptRoot 'lib/Ed25519.ps1')

$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }
function Assert-False { param($Condition, [string] $Message) if ($Condition) { throw $Message } }
function Assert-Equal {
    param($Expected, $Actual, [string] $Message)
    if ("$Expected" -ne "$Actual") { throw "$Message (expected '$Expected', got '$Actual')" }
}

function Get-Bytes { param([string] $Hex) if ($Hex.Length -eq 0) { return , (New-Object byte[] 0) } return [System.Convert]::FromHexString($Hex) }

# RFC 8032 section 7.1, tests 1 to 3.
$script:Vectors = @(
    @{
        Name      = 'RFC 8032 test 1 (empty message)'
        Seed      = '9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'
        PublicKey = 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'
        Message   = ''
        Signature = 'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'
    },
    @{
        Name      = 'RFC 8032 test 2 (one byte)'
        Seed      = '4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb'
        PublicKey = '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c'
        Message   = '72'
        Signature = '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00'
    },
    @{
        Name      = 'RFC 8032 test 3 (two bytes)'
        Seed      = 'c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7'
        PublicKey = 'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025'
        Message   = 'af82'
        Signature = '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a'
    }
)

Write-Host 'Ed25519.ps1'

foreach ($vector in $script:Vectors) {
    $case = $vector
    Test-Case "$($case.Name): public key derives from the seed" {
        Assert-Equal $case.PublicKey `
            (ConvertTo-Ed25519Hex -Bytes (Get-Ed25519PublicKey -Seed (Get-Bytes $case.Seed))) `
            'derived public key'
    }
    Test-Case "$($case.Name): signature matches the vector" {
        Assert-Equal $case.Signature `
            (New-Ed25519Signature -Seed (Get-Bytes $case.Seed) -Message (Get-Bytes $case.Message)) `
            'produced signature'
    }
    Test-Case "$($case.Name): the vector's signature verifies" {
        Assert-True (Test-Ed25519Signature -Message (Get-Bytes $case.Message) `
                -SignatureHex $case.Signature -PublicKeyHex $case.PublicKey) 'should verify'
    }
}

$script:Good = $script:Vectors[2]

Test-Case 'a changed message does not verify' {
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes 'af83') `
            -SignatureHex $script:Good.Signature -PublicKeyHex $script:Good.PublicKey) 'must refuse'
}

Test-Case 'a changed signature byte does not verify' {
    $tampered = $script:Good.Signature.Substring(0, 126) + 'ff'
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex $tampered -PublicKeyHex $script:Good.PublicKey) 'must refuse'
}

Test-Case 'a valid signature under a different key does not verify' {
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex $script:Good.Signature -PublicKeyHex $script:Vectors[1].PublicKey) 'must refuse'
}

Test-Case 'a truncated or non-hex signature is refused, not thrown on' {
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex 'deadbeef' -PublicKeyHex $script:Good.PublicKey) 'truncated'
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex ('z' * 128) -PublicKeyHex $script:Good.PublicKey) 'non-hex'
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex $script:Good.Signature -PublicKeyHex '') 'missing public key'
}

Test-Case 'an s value at or above the group order is refused' {
    # The low half stays the vector's R; the high half becomes the group order itself,
    # which is congruent to zero and would verify under an implementation that reduced
    # instead of refusing.
    $order = [System.Numerics.BigInteger]::Pow(2, 252) +
    [System.Numerics.BigInteger]::Parse('27742317777372353535851937790883648493')
    $sBytes = ConvertTo-Ed25519LittleEndian -Value $order -Length 32
    $malleable = $script:Good.Signature.Substring(0, 64) + (ConvertTo-Ed25519Hex -Bytes $sBytes)
    Assert-False (Test-Ed25519Signature -Message (Get-Bytes $script:Good.Message) `
            -SignatureHex $malleable -PublicKeyHex $script:Good.PublicKey) 'must refuse'
}

Test-Case 'a signature over real file bytes round-trips' {
    $seed = Get-Bytes $script:Good.Seed
    $message = [System.Text.Encoding]::UTF8.GetBytes("{`n  `"schema`": `"exosnap.release-verification/1`"`n}`n")
    $signature = New-Ed25519Signature -Seed $seed -Message $message
    Assert-True (Test-Ed25519Signature -Message $message -SignatureHex $signature `
            -PublicKeyHex (ConvertTo-Ed25519Hex -Bytes (Get-Ed25519PublicKey -Seed $seed))) 'round trip'
}

Write-Host ''
Write-Host "Ed25519: $script:Passed passed, $script:Failed failed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
