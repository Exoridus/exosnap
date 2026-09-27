fn main() {
    // Media Foundation is absent from Windows N editions without the Media
    // Feature Pack, and Windows Sandbox inherits the host's edition. A static
    // import of it would stop the verifier from starting at all there, even for
    // lanes that never decode media; delay-loading defers the failure to the
    // media code that needs it.
    let target = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") && target == "msvc" {
        for dll in ["mf.dll", "mfplat.dll"] {
            println!("cargo:rustc-link-arg-bins=/DELAYLOAD:{dll}");
        }
        println!("cargo:rustc-link-arg-bins=delayimp.lib");
    }
}
