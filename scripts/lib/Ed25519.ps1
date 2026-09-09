#Requires -Version 7.0
<#
.SYNOPSIS
    Ed25519 (RFC 8032) signing and detached-signature verification, in PowerShell
    with no external dependency.

.DESCRIPTION
    The release pipeline already signs the update manifest with an ed25519 key, and
    the qualification record is signed with the same key so the publish lock can tell
    a record the campaign produced from one a person typed. That verification runs
    inside `scripts/check-release-qualification.ps1`, on a CI runner, before anything
    is published, and the local `release-verify.ps1 qualify -Publish` produces the
    signature on the developer's machine.

    Both ends therefore had to work with nothing installed. The manifest signer uses
    python3 and PyNaCl, which is fine inside one workflow step but would put a pip
    package on the critical path of the gate, of every script test, and of every
    developer machine. .NET exposes no Ed25519 primitive, so the curve arithmetic is
    here.

    Not constant-time: the scalar ladder branches on key bits. The signing key never
    leaves the machine that holds it and no attacker-controlled process shares that
    timing, so the property this code needs is correctness, which RFC 8032's test
    vectors establish (scripts/tests/ed25519.tests.ps1).

    Keys and signatures use the wire encodings the rest of the pipeline already
    speaks: a 32-byte seed (base64, as the EXOSNAP_UPDATE_SIGNING_KEY secret holds
    it), a 32-byte public key (lowercase hex, as EXOSNAP_UPDATE_PUBLIC_KEY_HEX holds
    it), and a 64-byte detached signature (lowercase hex, as the `.sig` sidecar of
    the update manifest holds it).
#>

$script:Ed25519P = [System.Numerics.BigInteger]::Pow(2, 255) - 19
$script:Ed25519L = [System.Numerics.BigInteger]::Pow(2, 252) +
[System.Numerics.BigInteger]::Parse('27742317777372353535851937790883648493')
$script:Ed25519D = [System.Numerics.BigInteger]::Parse(
    '37095705934669439343138083508754565189542113879843219016388785533085940283555')
$script:Ed25519BaseX = [System.Numerics.BigInteger]::Parse(
    '15112221349535400772501151409588531511454012693041857206046113283949847762202')
$script:Ed25519BaseY = [System.Numerics.BigInteger]::Parse(
    '46316835694926478169428394003475163141307993866256225615783033603165251855960')

function Get-Ed25519Mod {
    <#
    .SYNOPSIS
        A non-negative residue. BigInteger's % keeps the sign of the dividend, which
        every formula below would otherwise have to correct for individually.
    #>
    param([System.Numerics.BigInteger] $Value, [System.Numerics.BigInteger] $Modulus)
    $result = $Value % $Modulus
    if ($result.Sign -lt 0) { $result += $Modulus }
    return $result
}

function Get-Ed25519Inverse {
    param([System.Numerics.BigInteger] $Value)
    return [System.Numerics.BigInteger]::ModPow(
        (Get-Ed25519Mod -Value $Value -Modulus $script:Ed25519P), $script:Ed25519P - 2, $script:Ed25519P)
}

function New-Ed25519Point {
    <#
    .SYNOPSIS
        A curve point in extended coordinates (X:Y:Z:T), the representation the
        addition formula below operates on.
    #>
    param(
        [System.Numerics.BigInteger] $X,
        [System.Numerics.BigInteger] $Y,
        [System.Numerics.BigInteger] $Z,
        [System.Numerics.BigInteger] $T
    )
    return @{ X = $X; Y = $Y; Z = $Z; T = $T }
}

function Get-Ed25519Identity {
    return New-Ed25519Point -X ([System.Numerics.BigInteger]::Zero) -Y ([System.Numerics.BigInteger]::One) `
        -Z ([System.Numerics.BigInteger]::One) -T ([System.Numerics.BigInteger]::Zero)
}

function Get-Ed25519BasePoint {
    return New-Ed25519Point -X $script:Ed25519BaseX -Y $script:Ed25519BaseY `
        -Z ([System.Numerics.BigInteger]::One) `
        -T (Get-Ed25519Mod -Value ($script:Ed25519BaseX * $script:Ed25519BaseY) -Modulus $script:Ed25519P)
}

function Add-Ed25519Point {
    <#
    .SYNOPSIS
        Twisted-Edwards addition (a = -1, extended coordinates).
    .DESCRIPTION
        Complete for this curve: d is a non-square modulo p, so no pair of points hits
        a zero denominator. Doubling therefore needs no separate formula, and the
        ladder below can call this for both operations without a special case that
        would only ever be exercised by a rare input.
    #>
    param($Left, $Right)

    # PowerShell variable names are case-insensitive, so a local named $p here would
    # silently overwrite a parameter named $P.
    $modulus = $script:Ed25519P
    $a = Get-Ed25519Mod -Value (($Left.Y - $Left.X) * ($Right.Y - $Right.X)) -Modulus $modulus
    $b = Get-Ed25519Mod -Value (($Left.Y + $Left.X) * ($Right.Y + $Right.X)) -Modulus $modulus
    $c = Get-Ed25519Mod -Value ($Left.T * 2 * $script:Ed25519D * $Right.T) -Modulus $modulus
    $d = Get-Ed25519Mod -Value ($Left.Z * 2 * $Right.Z) -Modulus $modulus
    $e = $b - $a
    $f = $d - $c
    $g = $d + $c
    $h = $b + $a
    return New-Ed25519Point `
        -X (Get-Ed25519Mod -Value ($e * $f) -Modulus $modulus) `
        -Y (Get-Ed25519Mod -Value ($g * $h) -Modulus $modulus) `
        -Z (Get-Ed25519Mod -Value ($f * $g) -Modulus $modulus) `
        -T (Get-Ed25519Mod -Value ($e * $h) -Modulus $modulus)
}

function Get-Ed25519ScalarMultiple {
    param($Point, [System.Numerics.BigInteger] $Scalar)

    $result = Get-Ed25519Identity
    $addend = $Point
    $k = $Scalar
    while ($k.Sign -gt 0) {
        if (-not $k.IsEven) { $result = Add-Ed25519Point -Left $result -Right $addend }
        $addend = Add-Ed25519Point -Left $addend -Right $addend
        $k = $k -shr 1
    }
    return $result
}

function ConvertTo-Ed25519LittleEndian {
    <#
    .SYNOPSIS
        A non-negative integer as exactly $Length little-endian bytes.
    #>
    param([System.Numerics.BigInteger] $Value, [int] $Length = 32)
    $bytes = New-Object byte[] $Length
    $v = $Value
    for ($i = 0; $i -lt $Length; $i++) {
        $bytes[$i] = [byte]($v -band 0xFF)
        $v = $v -shr 8
    }
    return , $bytes
}

function ConvertFrom-Ed25519LittleEndian {
    param([byte[]] $Bytes)
    $value = [System.Numerics.BigInteger]::Zero
    for ($i = $Bytes.Length - 1; $i -ge 0; $i--) {
        $value = ($value -shl 8) + [System.Numerics.BigInteger]$Bytes[$i]
    }
    return $value
}

function ConvertTo-Ed25519EncodedPoint {
    param($Point)
    $inverseZ = Get-Ed25519Inverse -Value $Point.Z
    $x = Get-Ed25519Mod -Value ($Point.X * $inverseZ) -Modulus $script:Ed25519P
    $y = Get-Ed25519Mod -Value ($Point.Y * $inverseZ) -Modulus $script:Ed25519P
    $bytes = ConvertTo-Ed25519LittleEndian -Value $y -Length 32
    if (-not $x.IsEven) { $bytes[31] = [byte]($bytes[31] -bor 0x80) }
    return , $bytes
}

function ConvertFrom-Ed25519EncodedPoint {
    <#
    .SYNOPSIS
        Decompresses a 32-byte point, or $null when those bytes are not a point.
    .DESCRIPTION
        A public key and the R half of a signature both arrive as untrusted bytes, so
        every rejection path matters: a y coordinate at or above p, an x with no
        square root, and the one encoding that names a negative zero are all refused
        rather than coerced into some nearby valid point.
    #>
    param([byte[]] $Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -ne 32) { return $null }
    $p = $script:Ed25519P
    $work = [byte[]]$Bytes.Clone()
    $sign = ($work[31] -band 0x80) -ne 0
    $work[31] = [byte]($work[31] -band 0x7F)
    $y = ConvertFrom-Ed25519LittleEndian -Bytes $work
    if ($y -ge $p) { return $null }

    $ySquared = Get-Ed25519Mod -Value ($y * $y) -Modulus $p
    $u = Get-Ed25519Mod -Value ($ySquared - 1) -Modulus $p
    $v = Get-Ed25519Mod -Value ($script:Ed25519D * $ySquared + 1) -Modulus $p
    $x = Get-Ed25519Mod -Value ($u * (Get-Ed25519Inverse -Value $v)) -Modulus $p
    $candidate = [System.Numerics.BigInteger]::ModPow($x, ($p + 3) / 8, $p)
    if ((Get-Ed25519Mod -Value ($candidate * $candidate) -Modulus $p) -ne $x) {
        # The other square root: sqrt(-1) * candidate. Present for exactly half the
        # residues, which is why the first exponentiation alone is not conclusive.
        $sqrtMinusOne = [System.Numerics.BigInteger]::ModPow(
            [System.Numerics.BigInteger]2, ($p - 1) / 4, $p)
        $candidate = Get-Ed25519Mod -Value ($candidate * $sqrtMinusOne) -Modulus $p
        if ((Get-Ed25519Mod -Value ($candidate * $candidate) -Modulus $p) -ne $x) { return $null }
    }
    if ($candidate.IsZero -and $sign) { return $null }
    if ($candidate.IsEven -eq $sign) { $candidate = $p - $candidate }

    return New-Ed25519Point -X $candidate -Y $y -Z ([System.Numerics.BigInteger]::One) `
        -T (Get-Ed25519Mod -Value ($candidate * $y) -Modulus $p)
}

function Get-Ed25519Sha512 {
    param([byte[]] $Bytes)
    $sha = [System.Security.Cryptography.SHA512]::Create()
    try { return $sha.ComputeHash($Bytes) }
    finally { $sha.Dispose() }
}

function Join-Ed25519Bytes {
    param([byte[][]] $Parts)
    $total = 0
    foreach ($part in $Parts) { $total += $part.Length }
    $joined = New-Object byte[] $total
    $offset = 0
    foreach ($part in $Parts) {
        [Array]::Copy($part, 0, $joined, $offset, $part.Length)
        $offset += $part.Length
    }
    return , $joined
}

function ConvertTo-Ed25519Hex {
    param([byte[]] $Bytes)
    return [System.Convert]::ToHexString($Bytes).ToLowerInvariant()
}

function ConvertFrom-Ed25519Hex {
    <#
    .SYNOPSIS
        Hex text as bytes, or $null when the text is not hex of the expected length.
    #>
    param([string] $Text, [int] $ExpectedLength)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $trimmed = $Text.Trim()
    if ($trimmed -notmatch "^[0-9a-fA-F]{$($ExpectedLength * 2)}$") { return $null }
    return , [System.Convert]::FromHexString($trimmed)
}

function Get-Ed25519PublicKey {
    <#
    .SYNOPSIS
        The 32-byte public key for a 32-byte seed.
    #>
    param([Parameter(Mandatory)] [byte[]] $Seed)

    if ($Seed.Length -ne 32) { throw "An ed25519 seed is 32 bytes; got $($Seed.Length)." }
    $h = Get-Ed25519Sha512 -Bytes $Seed
    $scalarBytes = [byte[]]$h[0..31]
    $scalarBytes[0] = [byte]($scalarBytes[0] -band 248)
    $scalarBytes[31] = [byte](($scalarBytes[31] -band 127) -bor 64)
    $scalar = ConvertFrom-Ed25519LittleEndian -Bytes $scalarBytes
    return ConvertTo-Ed25519EncodedPoint -Point (Get-Ed25519ScalarMultiple -Point (Get-Ed25519BasePoint) -Scalar $scalar)
}

function New-Ed25519Signature {
    <#
    .SYNOPSIS
        A detached 64-byte Ed25519 signature over the exact bytes given.
    .OUTPUTS
        The signature as lowercase hex, the encoding the `.sig` sidecars use.
    #>
    param(
        [Parameter(Mandatory)] [byte[]] $Seed,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [byte[]] $Message
    )

    if ($Seed.Length -ne 32) { throw "An ed25519 seed is 32 bytes; got $($Seed.Length)." }
    $h = Get-Ed25519Sha512 -Bytes $Seed
    $scalarBytes = [byte[]]$h[0..31]
    $scalarBytes[0] = [byte]($scalarBytes[0] -band 248)
    $scalarBytes[31] = [byte](($scalarBytes[31] -band 127) -bor 64)
    $scalar = ConvertFrom-Ed25519LittleEndian -Bytes $scalarBytes
    $prefix = [byte[]]$h[32..63]
    $publicKey = ConvertTo-Ed25519EncodedPoint -Point (
        Get-Ed25519ScalarMultiple -Point (Get-Ed25519BasePoint) -Scalar $scalar)

    $r = Get-Ed25519Mod -Value (ConvertFrom-Ed25519LittleEndian -Bytes (
            Get-Ed25519Sha512 -Bytes (Join-Ed25519Bytes -Parts @($prefix, $Message)))) -Modulus $script:Ed25519L
    $rPoint = ConvertTo-Ed25519EncodedPoint -Point (
        Get-Ed25519ScalarMultiple -Point (Get-Ed25519BasePoint) -Scalar $r)
    $k = Get-Ed25519Mod -Value (ConvertFrom-Ed25519LittleEndian -Bytes (
            Get-Ed25519Sha512 -Bytes (Join-Ed25519Bytes -Parts @($rPoint, $publicKey, $Message)))) -Modulus $script:Ed25519L
    $s = Get-Ed25519Mod -Value ($r + $k * $scalar) -Modulus $script:Ed25519L

    return ConvertTo-Ed25519Hex -Bytes (Join-Ed25519Bytes -Parts @(
            $rPoint, (ConvertTo-Ed25519LittleEndian -Value $s -Length 32)))
}

function Test-Ed25519Signature {
    <#
    .SYNOPSIS
        Whether a detached signature verifies over these exact bytes under this key.
    .DESCRIPTION
        Returns $false for every kind of bad input rather than throwing, because every
        caller is a gate whose answer is "do not publish" either way, and a thrown
        exception is one more path that has to be remembered to catch.
    .PARAMETER SignatureHex
        128 hex characters, as written into a `.sig` sidecar.
    .PARAMETER PublicKeyHex
        64 hex characters, as EXOSNAP_UPDATE_PUBLIC_KEY_HEX carries them.
    #>
    param(
        [byte[]] $Message,
        [string] $SignatureHex,
        [string] $PublicKeyHex
    )

    if ($null -eq $Message) { return $false }
    $signature = ConvertFrom-Ed25519Hex -Text $SignatureHex -ExpectedLength 64
    if ($null -eq $signature) { return $false }
    $publicKey = ConvertFrom-Ed25519Hex -Text $PublicKeyHex -ExpectedLength 32
    if ($null -eq $publicKey) { return $false }

    $rBytes = [byte[]]$signature[0..31]
    $s = ConvertFrom-Ed25519LittleEndian -Bytes ([byte[]]$signature[32..63])
    # A signature carrying an s at or above the group order encodes the same point as
    # a smaller one, so accepting it would make signatures malleable.
    if ($s -ge $script:Ed25519L) { return $false }

    $rPoint = ConvertFrom-Ed25519EncodedPoint -Bytes $rBytes
    if ($null -eq $rPoint) { return $false }
    $aPoint = ConvertFrom-Ed25519EncodedPoint -Bytes $publicKey
    if ($null -eq $aPoint) { return $false }

    $k = Get-Ed25519Mod -Value (ConvertFrom-Ed25519LittleEndian -Bytes (
            Get-Ed25519Sha512 -Bytes (Join-Ed25519Bytes -Parts @($rBytes, $publicKey, $Message)))) -Modulus $script:Ed25519L
    $left = Get-Ed25519ScalarMultiple -Point (Get-Ed25519BasePoint) -Scalar $s
    $right = Add-Ed25519Point -Left $rPoint -Right (Get-Ed25519ScalarMultiple -Point $aPoint -Scalar $k)

    $leftBytes = ConvertTo-Ed25519EncodedPoint -Point $left
    $rightBytes = ConvertTo-Ed25519EncodedPoint -Point $right
    for ($i = 0; $i -lt 32; $i++) {
        if ($leftBytes[$i] -ne $rightBytes[$i]) { return $false }
    }
    return $true
}
