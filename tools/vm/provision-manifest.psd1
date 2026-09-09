@{
    # Every piece of third-party software the release-verification guest runs, with
    # the pin that decides which bytes it is. Nothing is installed "latest": a golden
    # image whose tool set drifts turns every disagreement between two campaigns into
    # an investigation of the image instead of the product.
    #
    # Two kinds of pin, and both end in a SHA-256 comparison:
    #
    #   winget    the package id plus an exact version. winget verifies the installer
    #             against the hash in its own pinned manifest, so the pin here is the
    #             version and the verification is winget's.
    #   download  a URL built from the version, plus the SHA-256 of the file at that
    #             URL. provision.ps1 compares before it runs anything.
    #
    # PIN-REQUIRED is not a placeholder to ignore. provision.ps1 refuses to install a
    # package whose version or hash still reads PIN-REQUIRED and prints the command
    # that produces the real value. The pins are recorded once, while the golden image
    # is built, and committed before the image is frozen -- see
    # docs/dev/release-verify-vm.md, "Pinning the manifest".

    schema   = 1

    packages = @(
        @{
            id      = 'vcredist'
            title   = 'Visual C++ 2015-2022 x64 runtime'
            kind    = 'winget'
            wingetId = 'Microsoft.VCRedist.2015+.x64'
            version = '14.51.36247.0'
            reason  = 'ExoSnap and its updater link the dynamic CRT'
        }
        @{
            id      = 'pwsh'
            title   = 'PowerShell 7'
            kind    = 'winget'
            wingetId = 'Microsoft.PowerShell'
            version = '7.6.5.0'
            reason  = 'every script in scripts/ declares #Requires -Version 7.0'
        }
        @{
            id      = 'ffmpeg'
            title   = 'FFmpeg tools (ffprobe)'
            kind    = 'winget'
            wingetId = 'Gyan.FFmpeg'
            version = '9.0.1'
            reason  = 'the independent oracle for every recorded file'
        }
        @{
            id          = 'presentmon'
            title       = 'Intel PresentMon CLI'
            kind        = 'download'
            urlTemplate = 'https://github.com/GameTechDev/PresentMon/releases/download/v{version}/PresentMon-{version}-x64.exe'
            version     = '2.5.1'
            sha256      = '9bec3083069f58f911e6a512f4806db51a27bd096103087bc1d05ef54c80a191'
            fileName    = 'PresentMon.exe'
            layout      = 'file'
            reason      = 'the independent present-mode oracle; ExoSnap agreeing with itself is not evidence'
        }
        @{
            id          = 'soundvolumeview'
            title       = 'NirSoft SoundVolumeView'
            kind        = 'download'
            urlTemplate = 'https://www.nirsoft.net/utils/soundvolumeview-x64.zip'
            version     = '2.53'
            sha256      = 'ee2c45553fb9fb31b71db88b537c18e25c1a387a4a9009d081bdb38265c68e1f'
            fileName    = 'SoundVolumeView.exe'
            layout      = 'zip'
            reason      = 'sets the default endpoint and its shared-mode format; its exit code is never the evidence'
        }
        @{
            id          = 'vbcable'
            title       = 'VB-CABLE virtual audio device'
            kind        = 'download'
            urlTemplate = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack{version}.zip'
            version     = '45'
            sha256      = 'b950e39f01af1d04ea623c8f6d8eb9b6ea5c477c637295fabf20631c85116bfb'
            layout      = 'zip'
            installer   = 'VBCABLE_Setup_x64.exe'
            rebootRequired = $true
            reason      = 'a render endpoint nothing is routed to, and a device pnputil may disable'
        }
        @{
            id          = 'idd'
            title       = 'Virtual Display Driver (indirect display driver)'
            kind        = 'download'
            urlTemplate = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/{version}/VirtualDisplayDriver-x86.Driver.Only.zip'
            version     = '25.7.23'
            sha256      = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'
            layout      = 'zip'
            # The root-enumerated device the driver binds to. It changed between
            # driver generations, so it travels with the version pin.
            hardwareId  = 'Root\MttVDD'
            reason      = 'a monitor with a fixed mode list. A GPU-partitioned guest has no display output of its own, and Output Duplication needs one'
        }
        @{
            id          = 'nefcon'
            title       = 'NefCon windowless device installer'
            kind        = 'download'
            urlTemplate = 'https://github.com/nefarius/nefcon/releases/download/v{version}/nefcon_v{version}.zip'
            version     = '1.14.0'
            sha256      = 'a15557da24a9efca203158de3b43b0eaf982db231f0194031f1ed428bc13e669'
            fileName    = 'nefconw.exe'
            layout      = 'zip'
            reason      = 'creates the root-enumerated device node and binds the signed indirect display driver without UI'
        }
    )

    # The virtual monitor the guest presents. Fixed on purpose: a gate that asserts a
    # resolution or a refresh rate must not depend on what the host happens to run.
    display  = @{
        width        = 2560
        height       = 1440
        refreshRates = @(60, 144)
        # Selectable rather than always on: HDR changes the capture colour space, so
        # the SDR and HDR gates want different golden states, not one compromise.
        hdr          = $false
        # Where the driver reads its mode list. The directory moved between driver
        # generations, so it is a manifest field rather than a constant in the script.
        configDirectory = 'C:\VirtualDisplayDriver'
    }

    # Where provisioning puts what it downloads, inside the guest.
    paths    = @{
        tools    = 'C:\ExoSnapTools'
        staging  = 'C:\ExoSnapProvision'
        # Copy-VMFile writes here from the host; provisioning moves the contents into
        # the protected system locations, which Copy-VMFile must not target directly.
        hostDriverStaging = 'C:\HostDriverStore'
    }
}
