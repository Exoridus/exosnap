#![cfg(windows)]

use std::path::Path;
use std::process::Command;

static MSI_SESSION_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[link(name = "msi")]
unsafe extern "system" {
    fn MsiOpenPackageExW(path: *const u16, options: u32, handle: *mut u32) -> u32;
    fn MsiGetActiveDatabase(handle: u32) -> u32;
    fn MsiCloseHandle(handle: u32) -> u32;
    fn MsiDatabaseOpenViewW(database: u32, query: *const u16, view: *mut u32) -> u32;
    fn MsiViewExecute(view: u32, record: u32) -> u32;
    fn MsiViewFetch(view: u32, record: *mut u32) -> u32;
    fn MsiRecordGetStringW(record: u32, field: u32, buffer: *mut u16, size: *mut u32) -> u32;
    fn MsiSetPropertyW(handle: u32, name: *const u16, value: *const u16) -> u32;
    fn MsiGetPropertyW(handle: u32, name: *const u16, buffer: *mut u16, size: *mut u32) -> u32;
    fn MsiEvaluateConditionW(handle: u32, condition: *const u16) -> u32;
    fn MsiDoActionW(handle: u32, action: *const u16) -> u32;
}

#[link(name = "ole32")]
unsafe extern "system" {
    fn CoInitializeEx(reserved: *mut std::ffi::c_void, flags: u32) -> i32;
    fn CoCreateGuid(guid: *mut windows::core::GUID) -> i32;
    fn CoUninitialize();
}

#[link(name = "advapi32")]
unsafe extern "system" {
    fn RegCreateKeyExW(
        key: *mut std::ffi::c_void,
        subkey: *const u16,
        reserved: u32,
        class: *mut u16,
        options: u32,
        access: u32,
        security: *const std::ffi::c_void,
        result: *mut *mut std::ffi::c_void,
        disposition: *mut u32,
    ) -> i32;
    fn RegCloseKey(key: *mut std::ffi::c_void) -> i32;
    fn RegDeleteTreeW(key: *mut std::ffi::c_void, subkey: *const u16) -> i32;
}

#[repr(C)]
struct NativeUnicodeString {
    length: u16,
    maximum_length: u16,
    buffer: *mut u16,
}

#[link(name = "ntdll")]
unsafe extern "system" {
    fn NtSetValueKey(
        key: *mut std::ffi::c_void,
        name: *const NativeUnicodeString,
        title: u32,
        kind: u32,
        data: *const u8,
        size: u32,
    ) -> i32;
    fn NtQueryValueKey(
        key: *mut std::ffi::c_void,
        name: *const NativeUnicodeString,
        information: u32,
        data: *mut u8,
        size: u32,
        result_size: *mut u32,
    ) -> i32;
}

fn native_value_name(name: &mut [u16]) -> NativeUnicodeString {
    NativeUnicodeString {
        length: ((name.len() - 1) * 2) as u16,
        maximum_length: (name.len() * 2) as u16,
        buffer: name.as_mut_ptr(),
    }
}

fn current_user() -> *mut std::ffi::c_void {
    0x80000001u32 as i32 as isize as *mut std::ffi::c_void
}

struct RegistryFixture {
    key: *mut std::ffi::c_void,
    path: String,
}

impl RegistryFixture {
    fn create() -> Self {
        let mut guid = windows::core::GUID::zeroed();
        assert_eq!(unsafe { CoCreateGuid(&mut guid) }, 0);
        let path = format!("Software\\ExoSnapTests\\{{{guid:?}}}");
        assert!(path.starts_with("Software\\ExoSnapTests\\{") && path.ends_with('}'));
        let mut key = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                RegCreateKeyExW(
                    current_user(),
                    wide(&path).as_ptr(),
                    0,
                    std::ptr::null_mut(),
                    0,
                    0x103,
                    std::ptr::null(),
                    &mut key,
                    std::ptr::null_mut(),
                )
            },
            0
        );
        Self { key, path }
    }

    fn set_owner(&self, value: &str) {
        let value = wide(value);
        let bytes = unsafe { std::slice::from_raw_parts(value.as_ptr().cast(), value.len() * 2) };
        self.set_value(1, bytes);
    }

    fn read_value(&self) -> (u32, Vec<u8>) {
        let mut name = wide("DistributionOwner");
        let name = native_value_name(&mut name);
        let mut buffer = vec![0u8; 1024];
        let mut size = 0;
        assert_eq!(
            unsafe {
                NtQueryValueKey(
                    self.key,
                    &name,
                    2,
                    buffer.as_mut_ptr(),
                    buffer.len() as u32,
                    &mut size,
                )
            },
            0
        );
        let kind = u32::from_le_bytes(buffer[4..8].try_into().unwrap());
        let length = u32::from_le_bytes(buffer[8..12].try_into().unwrap()) as usize;
        assert!(12 + length <= size as usize && size as usize <= buffer.len());
        (kind, buffer[12..12 + length].to_vec())
    }

    fn set_value(&self, kind: u32, value: &[u8]) {
        let mut name = wide("DistributionOwner");
        let name = native_value_name(&mut name);
        // Win32 string writes may append a NUL. Native writes preserve exact
        // malformed bytes so the probe cannot pass against normalized fixtures.
        assert_eq!(
            unsafe { NtSetValueKey(self.key, &name, 0, kind, value.as_ptr(), value.len() as u32) },
            0
        );
    }
}

impl Drop for RegistryFixture {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.key) };
        assert!(self.path.starts_with("Software\\ExoSnapTests\\{") && self.path.ends_with('}'));
        assert_eq!(
            unsafe { RegDeleteTreeW(current_user(), wide(&self.path).as_ptr()) },
            0
        );
    }
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryW(path: *const u16) -> *mut std::ffi::c_void;
    fn GetProcAddress(module: *mut std::ffi::c_void, name: *const u8) -> *mut std::ffi::c_void;
    fn FreeLibrary(module: *mut std::ffi::c_void) -> i32;
}

struct ProbeLibrary(*mut std::ffi::c_void);
impl ProbeLibrary {
    fn open(path: &Path) -> Self {
        let module = unsafe { LoadLibraryW(wide(&path.to_string_lossy()).as_ptr()) };
        assert!(!module.is_null(), "failed to load probe DLL");
        Self(module)
    }

    fn run(&self, session: &Handle) {
        let address = unsafe { GetProcAddress(self.0, c"ProbeDistributionOwner".as_ptr().cast()) };
        assert!(!address.is_null());
        let probe: unsafe extern "system" fn(u32) -> u32 = unsafe { std::mem::transmute(address) };
        assert_eq!(unsafe { probe(session.0) }, 0);
    }
}
impl Drop for ProbeLibrary {
    fn drop(&mut self) {
        unsafe { FreeLibrary(self.0) };
    }
}

fn build_probe(root: &Path, scratch: &Path, registry_key: Option<&str>) -> std::path::PathBuf {
    let dll = scratch.join("exosnap-msi-owner-probe.dll");
    let mut command = Command::new("cl.exe");
    command.args(["/nologo", "/std:c++20", "/W4", "/WX", "/MT", "/LD"]);
    if let Some(key) = registry_key {
        command.arg("/DEXOSNAP_OWNER_PROBE_TEST_ROOT=HKEY_CURRENT_USER");
        command.arg(format!(
            "/DEXOSNAP_OWNER_PROBE_TEST_KEY=L\"{}\"",
            key.replace('\\', "\\\\")
        ));
    }
    let output = command
        .arg(root.join("packaging/msi/DistributionOwnerProbe.cpp"))
        .arg("/link")
        .args(["msi.lib", "advapi32.lib"])
        .arg(format!("/OUT:{}", dll.display()))
        .current_dir(scratch)
        .output()
        .expect("requires x64 MSVC developer environment");
    assert!(
        output.status.success(),
        "probe build failed: {}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    dll
}
struct Com;
impl Drop for Com {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

struct Handle(u32);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { MsiCloseHandle(self.0) };
    }
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

fn rows(database: &Handle, query: &str, columns: u32) -> Vec<Vec<String>> {
    unsafe {
        let mut view = 0;
        assert_eq!(
            MsiDatabaseOpenViewW(database.0, wide(query).as_ptr(), &mut view),
            0,
            "{query}"
        );
        let view = Handle(view);
        assert_eq!(MsiViewExecute(view.0, 0), 0);
        let mut result = Vec::new();
        loop {
            let mut record = 0;
            let code = MsiViewFetch(view.0, &mut record);
            if code == 259 {
                break;
            }
            assert_eq!(code, 0);
            let record = Handle(record);
            result.push(
                (1..=columns)
                    .map(|field| {
                        let mut buffer = vec![0u16; 4096];
                        let mut length = buffer.len() as u32;
                        assert_eq!(
                            MsiRecordGetStringW(record.0, field, buffer.as_mut_ptr(), &mut length),
                            0
                        );
                        String::from_utf16(&buffer[..length as usize]).unwrap()
                    })
                    .collect(),
            );
        }
        result
    }
}

fn set(session: &Handle, name: &str, value: &str) {
    assert_eq!(
        unsafe { MsiSetPropertyW(session.0, wide(name).as_ptr(), wide(value).as_ptr()) },
        0
    );
}

fn get(session: &Handle, name: &str) -> String {
    let mut buffer = vec![0u16; 4096];
    let mut length = buffer.len() as u32;
    assert_eq!(
        unsafe {
            MsiGetPropertyW(
                session.0,
                wide(name).as_ptr(),
                buffer.as_mut_ptr(),
                &mut length,
            )
        },
        0
    );
    String::from_utf16(&buffer[..length as usize]).unwrap()
}

fn build_msi(root: &Path, scratch: &Path) -> std::path::PathBuf {
    let probe = build_probe(root, scratch, None);
    build_msi_source(
        root,
        scratch,
        &root.join("packaging/msi/Package.wxs"),
        &probe,
    )
}

fn build_msi_source(
    root: &Path,
    scratch: &Path,
    source: &Path,
    probe: &Path,
) -> std::path::PathBuf {
    let harvest = scratch.join("harvest.wxs");
    std::fs::write(&harvest, r#"<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Fragment><ComponentGroup Id="StagingFiles" Directory="INSTALLFOLDER"><Component><File Source="$(var.DummyPath)" Name="exosnap.exe" /></Component></ComponentGroup></Fragment></Wix>"#).unwrap();
    let dummy = scratch.join("exosnap.exe");
    std::fs::write(&dummy, "ownership fixture").unwrap();
    let msi = scratch.join("ownership.msi");
    let wix = std::env::var_os("EXOSNAP_TEST_WIX")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::path::PathBuf::from(std::env::var_os("USERPROFILE").expect("USERPROFILE"))
                .join(".dotnet/tools/wix.exe")
        });
    let extension = std::env::var_os("EXOSNAP_TEST_WIX_UTIL")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::path::PathBuf::from(std::env::var_os("USERPROFILE").expect("USERPROFILE")).join(
                ".wix/extensions/WixToolset.Util.wixext/4.0.5/wixext4/WixToolset.Util.wixext.dll",
            )
        });
    let output = Command::new(wix)
        .args(["build", "-arch", "x64", "-ext"])
        .arg(extension)
        .args([
            "-d",
            "ProductVersion=0.10.1",
            "-d",
            "VCRedistMinVersion=14.44.35211.0",
            "-d",
        ])
        .arg(format!(
            "AppIconPath={}",
            root.join("app/assets/brand/exosnap-app.ico").display()
        ))
        .arg("-d")
        .arg(format!("DummyPath={}", dummy.display()))
        .arg("-d")
        .arg(format!("DistributionOwnerProbePath={}", probe.display()))
        .arg(source)
        .arg(harvest)
        .arg("-o")
        .arg(&msi)
        .current_dir(scratch)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "WiX build failed: {}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    msi
}

fn build_registry_msi(
    root: &Path,
    scratch: &Path,
    registry: &RegistryFixture,
    probe: &Path,
    extra_probe: bool,
) -> std::path::PathBuf {
    let source = std::fs::read_to_string(root.join("packaging/msi/Package.wxs")).unwrap();
    let doc = roxmltree::Document::parse(&source).unwrap();
    let node = doc
        .descendants()
        .find(|n| n.attribute("Id") == Some("ExoSnapDistributionOwnerRemembered"))
        .unwrap();
    let search = &source[node.range()];
    let mut fixture = source.replace(
        search,
        &search.replace("Root=\"HKLM\"", "Root=\"HKCU\"").replace(
            "Key=\"Software\\ExoSnap\"",
            &format!("Key=\"{}\"", registry.path),
        ),
    );
    // The fixture retains the production search schema and resolver actions,
    // but AppSearch can read only the unique scratch key, never product state.
    for node in doc.descendants().filter(|n| {
        (n.has_tag_name("RegistrySearch") || n.has_tag_name("DirectorySearch"))
            && n.attribute("Id") != Some("ExoSnapDistributionOwnerRemembered")
    }) {
        fixture = fixture.replace(&source[node.range()], "");
    }
    if extra_probe {
        let package = doc
            .descendants()
            .find(|n| n.has_tag_name("Package"))
            .unwrap();
        let package_end = &source[package.range().end - "</Package>".len()..package.range().end];
        fixture = fixture.replace(package_end, &format!(r#"<Property Id="EXOSNAP_TEST_OWNER_PROBE" Secure="yes"><RegistrySearchRef Id="ExoSnapDistributionOwnerRemembered" /></Property>{package_end}"#));
    }
    let fixture_path = scratch.join("Package.wxs");
    std::fs::write(&fixture_path, fixture).unwrap();
    build_msi_source(root, scratch, &fixture_path, probe)
}

#[test]
#[ignore = "requires x64 MSVC, WiX 4 and Util extension; opens only a restricted MSI property session"]
fn compiled_msi_preserves_distribution_ownership_without_installing() {
    let _session_lock = MSI_SESSION_LOCK.lock().unwrap();
    assert!(unsafe { CoInitializeEx(std::ptr::null_mut(), 2) } >= 0);
    let _com = Com;
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let scratch = tempfile::tempdir().unwrap();
    let msi = build_msi(&root, scratch.path());
    let mut session = 0;
    // IGNOREMACHINESTATE confines this session to property operations. No
    // standard actions or package-install API is called by this test.
    assert_eq!(
        unsafe { MsiOpenPackageExW(wide(&msi.to_string_lossy()).as_ptr(), 1, &mut session) },
        0
    );
    let session = Handle(session);
    let database = Handle(unsafe { MsiGetActiveDatabase(session.0) });
    assert_ne!(database.0, 0);
    let registry = rows(
        &database,
        "SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`",
        5,
    );
    let owner = registry
        .iter()
        .find(|row| row[2] == "DistributionOwner")
        .expect("compiled MSI persists ownership");
    assert_eq!(
        &owner[..4],
        [
            "2",
            "Software\\ExoSnap",
            "DistributionOwner",
            "[EXOSNAP_RESOLVED_DISTRIBUTION_OWNER]"
        ]
    );
    for name in ["installed", "InstallPath"] {
        assert!(
            registry
                .iter()
                .any(|row| row[1] == owner[1] && row[2] == name && row[4] == owner[4])
        );
    }
    let component = rows(
        &database,
        &format!(
            "SELECT `Attributes` FROM `Component` WHERE `Component`='{}'",
            owner[4]
        ),
        1,
    );
    assert_ne!(
        component[0][0].parse::<u32>().unwrap() & 256,
        0,
        "ownership registry component must be 64-bit"
    );
    let locator = rows(
        &database,
        "SELECT `Root`, `Key`, `Name`, `Type` FROM `RegLocator` WHERE `Signature_`='ExoSnapDistributionOwnerRemembered'",
        4,
    );
    assert_eq!(
        locator,
        [vec!["2", "Software\\ExoSnap", "DistributionOwner", "18"]
            .into_iter()
            .map(String::from)
            .collect::<Vec<_>>()]
    );
    assert_eq!(
        rows(
            &database,
            "SELECT `Property` FROM `AppSearch` WHERE `Signature_`='ExoSnapDistributionOwnerRemembered'",
            1
        ),
        [vec!["EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED".to_string()]]
    );
    assert_eq!(get(&session, "EXOSNAP_DISTRIBUTION_OWNER"), "");
    assert!(
        get(&session, "SecureCustomProperties")
            .split(';')
            .any(|name| name == "EXOSNAP_DISTRIBUTION_OWNER")
    );

    assert_eq!(
        rows(
            &database,
            "SELECT `Type`, `Source`, `Target` FROM `CustomAction` WHERE `Action`='ProbeDistributionOwner'",
            3
        ),
        [vec![
            "1".to_string(),
            "DistributionOwnerProbe".to_string(),
            "ProbeDistributionOwner".to_string()
        ]]
    );
    assert_eq!(
        rows(
            &database,
            "SELECT `Name` FROM `Binary` WHERE `Name`='DistributionOwnerProbe'",
            1
        ),
        [vec!["DistributionOwnerProbe".to_string()]]
    );
    for table in ["InstallUISequence", "InstallExecuteSequence"] {
        let sequence = rows(
            &database,
            &format!("SELECT `Action`, `Sequence` FROM `{table}`"),
            2,
        );
        let number = |name: &str| {
            sequence.iter().find(|row| row[0] == name).unwrap()[1]
                .parse::<i32>()
                .unwrap()
        };
        assert!(number("AppSearch") < number("ProbeDistributionOwner"));
        assert!(number("ProbeDistributionOwner") < number("ResolveDistributionOwnerFromCaller"));
        assert!(number("ProbeDistributionOwner") < number("LaunchConditions"));
        if table == "InstallExecuteSequence" {
            assert!(number("ProbeDistributionOwner") < number("RemoveExistingProducts"));
        }
    }

    let names = [
        "ResolveDistributionOwnerFromCaller",
        "ResolveDistributionOwnerFromRegistry",
        "ResolveDistributionOwnerUnknown",
        "ResolveDistributionOwnerDefault",
    ];
    let mut execute_actions = Vec::new();
    for table in ["InstallUISequence", "InstallExecuteSequence"] {
        let sequence = rows(
            &database,
            &format!("SELECT `Action`, `Condition`, `Sequence` FROM `{table}` ORDER BY `Sequence`"),
            3,
        );
        let number = |name: &str| {
            sequence.iter().find(|row| row[0] == name).unwrap()[2]
                .parse::<i32>()
                .unwrap()
        };
        for name in names {
            assert!(number("AppSearch") < number(name));
            assert!(number(name) < number("LaunchConditions"));
            if table == "InstallExecuteSequence" {
                assert!(number(name) < number("RemoveExistingProducts"));
                execute_actions.push(sequence.iter().find(|row| row[0] == name).unwrap().clone());
            }
            let action = rows(
                &database,
                &format!("SELECT `Type`, `Source` FROM `CustomAction` WHERE `Action`='{name}'"),
                2,
            );
            assert_eq!(
                action,
                [vec![
                    "51".to_string(),
                    "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER".to_string()
                ]]
            );
        }
    }
    let launch = rows(&database, "SELECT `Condition` FROM `LaunchCondition`", 1)
        .into_iter()
        .find(|row| row[0].contains("EXOSNAP_DISTRIBUTION_OWNER"))
        .unwrap();
    for (caller, remembered, expected, allowed) in [
        ("", "", "direct", true),
        ("", "winget", "winget", true),
        ("", "chocolatey", "chocolatey", true),
        ("direct", "", "direct", true),
        ("winget", "", "winget", true),
        ("chocolatey", "", "chocolatey", true),
        ("direct", "winget", "direct", true),
        ("winget", "chocolatey", "winget", true),
        ("chocolatey", "winget", "chocolatey", true),
        ("", "unrecognized", "unrecognized", true),
        ("WINGET", "winget", "WINGET", false),
        ("other", "", "other", false),
        ("winget ", "", "winget ", false),
    ] {
        for (installed, upgrading) in [
            ("", ""),
            ("1", ""),
            ("", "{11111111-2222-3333-4444-555555555555}"),
        ] {
            set(&session, "Installed", installed);
            set(&session, "WIX_UPGRADE_DETECTED", upgrading);
            set(&session, "EXOSNAP_DISTRIBUTION_OWNER", caller);
            set(
                &session,
                "EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED",
                remembered,
            );
            set(
                &session,
                "EXOSNAP_DISTRIBUTION_OWNER_STRING",
                if remembered.is_empty() { "" } else { "1" },
            );
            set(&session, "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER", "stale");
            assert_eq!(
                unsafe { MsiEvaluateConditionW(session.0, wide(&launch[0]).as_ptr()) },
                u32::from(allowed),
                "caller={caller:?} remembered={remembered:?}"
            );
            for action in &execute_actions {
                match unsafe { MsiEvaluateConditionW(session.0, wide(&action[1]).as_ptr()) } {
                    0 => {}
                    1 => assert_eq!(
                        unsafe { MsiDoActionW(session.0, wide(&action[0]).as_ptr()) },
                        0
                    ),
                    result => panic!("unexpected MSI condition result {result}"),
                }
            }
            assert_eq!(
                get(&session, "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER"),
                expected
            );
        }
    }
}

#[test]
#[ignore = "requires x64 MSVC and WiX 4; creates and removes only a unique HKCU Software\\ExoSnapTests key"]
fn compiled_appsearch_cannot_distinguish_empty_owner_from_missing() {
    let _session_lock = MSI_SESSION_LOCK.lock().unwrap();
    assert!(unsafe { CoInitializeEx(std::ptr::null_mut(), 2) } >= 0);
    let _com = Com;
    let registry = RegistryFixture::create();
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let scratch = tempfile::tempdir().unwrap();
    let probe = build_probe(&root, scratch.path(), Some(&registry.path));
    let msi = build_registry_msi(&root, scratch.path(), &registry, &probe, true);
    let sentinel_a = "__EXOSNAP_OWNER_ABSENT_A__";
    let sentinel_b = "__EXOSNAP_OWNER_ABSENT_B__";
    for owner in [
        None,
        Some(""),
        Some("winget"),
        Some("chocolatey"),
        Some("direct"),
        Some("unrecognized"),
        Some(sentinel_a),
        Some(sentinel_b),
    ] {
        if let Some(owner) = owner {
            registry.set_owner(owner);
        }
        for seeded in [true, false] {
            let mut session = 0;
            assert_eq!(
                unsafe {
                    MsiOpenPackageExW(wide(&msi.to_string_lossy()).as_ptr(), 1, &mut session)
                },
                0
            );
            let session = Handle(session);
            let database = Handle(unsafe { MsiGetActiveDatabase(session.0) });
            assert_eq!(
                rows(
                    &database,
                    "SELECT `Root`, `Key`, `Name`, `Type` FROM `RegLocator`",
                    4
                ),
                [vec![
                    "1".to_string(),
                    registry.path.clone(),
                    "DistributionOwner".to_string(),
                    "18".to_string()
                ]]
            );
            assert_eq!(
                rows(&database, "SELECT `Property` FROM `AppSearch`", 1).len(),
                2
            );
            set(&session, "EXOSNAP_DISTRIBUTION_OWNER", "");
            set(
                &session,
                "EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED",
                if seeded { sentinel_a } else { "" },
            );
            set(
                &session,
                "EXOSNAP_TEST_OWNER_PROBE",
                if seeded { sentinel_b } else { "" },
            );
            assert_eq!(
                unsafe { MsiDoActionW(session.0, wide("AppSearch").as_ptr()) },
                0
            );
            let remembered = get(&session, "EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED");
            let probe = get(&session, "EXOSNAP_TEST_OWNER_PROBE");
            if let Some(owner) = owner.filter(|owner| !owner.is_empty()) {
                assert_eq!(remembered, owner);
                assert_eq!(probe, owner);
            } else {
                // AppSearch skips an empty REG_SZ instead of clearing the
                // property. Even distinct sentinels cannot prove existence.
                assert_eq!(remembered, if seeded { sentinel_a } else { "" });
                assert_eq!(probe, if seeded { sentinel_b } else { "" });
            }
            eprintln!(
                "owner={owner:?} seeded={seeded}: remembered={remembered:?}, probe={probe:?}"
            );
        }
    }
}

fn registry_string(value: &str) -> Vec<u8> {
    wide(value).into_iter().flat_map(u16::to_le_bytes).collect()
}

#[test]
#[ignore = "requires x64 MSVC and WiX 4; uses only a unique HKCU Software\\ExoSnapTests key"]
fn compiled_msi_probe_preserves_empty_and_unknown_owners_without_installing() {
    let _session_lock = MSI_SESSION_LOCK.lock().unwrap();
    assert!(unsafe { CoInitializeEx(std::ptr::null_mut(), 2) } >= 0);
    let _com = Com;
    let registry = RegistryFixture::create();
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let scratch = tempfile::tempdir().unwrap();
    let dll = build_probe(&root, scratch.path(), Some(&registry.path));
    let probe = ProbeLibrary::open(&dll);
    let msi = build_registry_msi(&root, scratch.path(), &registry, &dll, false);
    for (label, value, expected) in [
        ("missing", None, "direct"),
        ("empty", Some((1, registry_string(""))), "unknown"),
        ("winget", Some((1, registry_string("winget"))), "winget"),
        (
            "chocolatey",
            Some((1, registry_string("chocolatey"))),
            "chocolatey",
        ),
        ("direct", Some((1, registry_string("direct"))), "direct"),
        (
            "unrecognized",
            Some((1, registry_string("unrecognized"))),
            "unrecognized",
        ),
        (
            "unterminated",
            Some((1, b"w\0i\0n\0g\0e\0t\0".to_vec())),
            "",
        ),
        (
            "embedded-suffix",
            Some((1, registry_string("winget\0other"))),
            "",
        ),
        (
            "unterminated-direct",
            Some((1, b"d\0i\0r\0e\0c\0t\0".to_vec())),
            "",
        ),
        (
            "embedded-direct-suffix",
            Some((1, registry_string("direct\0other"))),
            "",
        ),
        (
            "trailing-nuls",
            Some((1, registry_string("winget\0"))),
            "winget",
        ),
        ("odd-size", Some((1, vec![b'w'])), ""),
        ("zero-size", Some((1, vec![])), ""),
        ("expand", Some((2, registry_string("winget"))), ""),
        ("dword", Some((4, 1u32.to_le_bytes().to_vec())), ""),
        ("binary", Some((3, b"winget".to_vec())), ""),
        ("multi", Some((7, registry_string("winget\0"))), ""),
    ] {
        if let Some((kind, data)) = &value {
            registry.set_value(*kind, data);
            assert_eq!(
                registry.read_value(),
                (*kind, data.clone()),
                "{label}: exact fixture bytes before AppSearch"
            );
        }
        let mut session = 0;
        assert_eq!(
            unsafe { MsiOpenPackageExW(wide(&msi.to_string_lossy()).as_ptr(), 1, &mut session) },
            0
        );
        let session = Handle(session);
        let database = Handle(unsafe { MsiGetActiveDatabase(session.0) });
        assert_eq!(
            rows(&database, "SELECT `Property` FROM `AppSearch`", 1),
            [vec!["EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED".to_string()]]
        );
        assert_eq!(
            rows(&database, "SELECT `Root`, `Key` FROM `RegLocator`", 2),
            [vec!["1".to_string(), registry.path.clone()]]
        );
        assert_eq!(
            unsafe { MsiDoActionW(session.0, wide("AppSearch").as_ptr()) },
            0
        );
        if let Some((kind, data)) = &value {
            assert_eq!(
                registry.read_value(),
                (*kind, data.clone()),
                "{label}: AppSearch must preserve exact fixture bytes"
            );
        }
        // Restricted MSI sessions reject Type 1 execution. Calling the actual
        // test-built export directly permits only its scratch-key read and
        // property write, without running an installer sequence.
        set(&session, "EXOSNAP_DISTRIBUTION_OWNER_PRESENT", "stale");
        probe.run(&session);
        assert_eq!(
            get(&session, "EXOSNAP_DISTRIBUTION_OWNER_PRESENT"),
            if value.is_some() { "1" } else { "" }
        );
        let remembered = get(&session, "EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED");
        assert_eq!(
            get(&session, "EXOSNAP_DISTRIBUTION_OWNER_STRING"),
            if value.as_ref().is_some_and(|(kind, _)| *kind == 1) && !expected.is_empty() {
                "1"
            } else {
                ""
            },
            "{label}: remembered={remembered:?}"
        );
        let retained = if expected.is_empty() {
            "unknown"
        } else {
            expected
        };
        let launch = rows(&database, "SELECT `Condition` FROM `LaunchCondition`", 1)
            .into_iter()
            .find(|row| row[0].contains("EXOSNAP_DISTRIBUTION_OWNER"))
            .unwrap();
        for (caller, allowed) in [
            ("", true),
            ("direct", true),
            ("winget", true),
            ("chocolatey", true),
            ("other", false),
            ("WINGET", false),
        ] {
            for (installed, upgrading) in [
                ("", ""),
                ("1", ""),
                ("", "{11111111-2222-3333-4444-555555555555}"),
            ] {
                set(&session, "Installed", installed);
                set(&session, "WIX_UPGRADE_DETECTED", upgrading);
                set(&session, "EXOSNAP_DISTRIBUTION_OWNER", caller);
                assert_eq!(
                    unsafe { MsiEvaluateConditionW(session.0, wide(&launch[0]).as_ptr()) },
                    u32::from(allowed)
                );
                for table in ["InstallUISequence", "InstallExecuteSequence"] {
                    set(&session, "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER", "stale");
                    let sequence = rows(
                        &database,
                        &format!("SELECT `Action`, `Condition` FROM `{table}` ORDER BY `Sequence`"),
                        2,
                    );
                    for action in sequence
                        .iter()
                        .filter(|row| row[0].starts_with("ResolveDistributionOwner"))
                    {
                        assert_eq!(
                            rows(
                                &database,
                                &format!(
                                    "SELECT `Type` FROM `CustomAction` WHERE `Action`='{}'",
                                    action[0]
                                ),
                                1
                            ),
                            [vec!["51".to_string()]]
                        );
                        match unsafe { MsiEvaluateConditionW(session.0, wide(&action[1]).as_ptr()) }
                        {
                            0 => {}
                            1 => assert_eq!(
                                unsafe { MsiDoActionW(session.0, wide(&action[0]).as_ptr()) },
                                0
                            ),
                            result => panic!("unexpected MSI condition result {result}"),
                        }
                    }
                    assert_eq!(
                        get(&session, "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER"),
                        if caller.is_empty() { retained } else { caller },
                        "{label}: {table} caller={caller:?} installed={installed:?} upgrading={upgrading:?}"
                    );
                }
            }
        }
        eprintln!(
            "{label}: present={}, remembered={remembered:?}, retained={retained:?}",
            get(&session, "EXOSNAP_DISTRIBUTION_OWNER_PRESENT")
        );
    }
    drop(registry);
    let mut session = 0;
    assert_eq!(
        unsafe { MsiOpenPackageExW(wide(&msi.to_string_lossy()).as_ptr(), 1, &mut session) },
        0
    );
    let session = Handle(session);
    assert_eq!(
        unsafe { MsiDoActionW(session.0, wide("AppSearch").as_ptr()) },
        0
    );
    set(&session, "EXOSNAP_DISTRIBUTION_OWNER_PRESENT", "stale");
    set(&session, "EXOSNAP_DISTRIBUTION_OWNER_STRING", "stale");
    probe.run(&session);
    assert_eq!(get(&session, "EXOSNAP_DISTRIBUTION_OWNER_PRESENT"), "");
    assert_eq!(get(&session, "EXOSNAP_DISTRIBUTION_OWNER_STRING"), "");
    let database = Handle(unsafe { MsiGetActiveDatabase(session.0) });
    let default = rows(
        &database,
        "SELECT `Condition` FROM `InstallExecuteSequence` WHERE `Action`='ResolveDistributionOwnerDefault'",
        1,
    );
    assert_eq!(
        unsafe { MsiEvaluateConditionW(session.0, wide(&default[0][0]).as_ptr()) },
        1
    );
    assert_eq!(
        unsafe { MsiDoActionW(session.0, wide("ResolveDistributionOwnerDefault").as_ptr()) },
        0
    );
    assert_eq!(
        get(&session, "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER"),
        "direct"
    );
}
