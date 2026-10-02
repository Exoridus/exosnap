//! Candidate-bound HTTPS release feed for disposable update verification.

use anyhow::{Context as _, Result, anyhow, ensure};
#[cfg(test)]
use ed25519_dalek::SigningKey;
use rustls::{ServerConfig, ServerConnection, StreamOwned};
use serde_json::json;
use std::io::{BufReader, Read, Seek, Write};
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::Arc;

use crate::bundle::{Bundle, FileRole, installer_name, portable_name, sha256_file};
use crate::manifest;

#[derive(clap::Args)]
pub struct FeedArgs {
    #[arg(long)]
    pub bundle: Option<PathBuf>,
    /// Manifest signed before the disposable guest starts.
    #[arg(long)]
    pub manifest: Option<PathBuf>,
    #[arg(long)]
    pub signature: Option<PathBuf>,
    #[arg(long)]
    pub cert: PathBuf,
    #[arg(long)]
    pub key: PathBuf,
    #[arg(long)]
    pub public_key_hex: Option<String>,
    /// Generate a disposable api.github.com TLS certificate and exit.
    #[arg(long)]
    pub generate_cert: bool,
    /// DER certificate for certutil import in the disposable guest.
    #[arg(long)]
    pub cert_der: Option<PathBuf>,
    #[arg(long, default_value = "127.0.0.1:443")]
    pub listen: String,
}

struct Response {
    content_type: &'static str,
    body: Body,
}

enum Body {
    Bytes(Vec<u8>),
    File(PathBuf, String, u64),
}

impl Response {
    #[cfg(test)]
    fn bytes(self) -> Result<Vec<u8>> {
        Ok(match self.body {
            Body::Bytes(bytes) => bytes,
            Body::File(path, _, _) => std::fs::read(path)?,
        })
    }
}

struct Feed {
    listing: Vec<u8>,
    manifest: Vec<u8>,
    signature: Vec<u8>,
    installer: PathBuf,
    portable: PathBuf,
    installer_hash: String,
    portable_hash: String,
    installer_size: u64,
    portable_size: u64,
    installer_path: String,
    portable_path: String,
}

impl Feed {
    #[cfg(test)]
    fn new(bundle: &Bundle, key: &SigningKey) -> Result<Self> {
        let version = &bundle.inventory.product_version;
        let installer_path = format!("/{}", installer_name(version));
        let portable_path = format!("/{}", portable_name(version));
        let url = |path: &str| format!("https://api.github.com{path}");
        let doc = manifest::for_bundle(bundle, &url(&installer_path), &url(&portable_path))?;
        let manifest = manifest::serialize(&doc)?;
        let signature = manifest::sign(&manifest, key).into_bytes();
        Self::from_signed(bundle, manifest, signature, &key.verifying_key())
    }

    fn from_signed(
        bundle: &Bundle,
        manifest: Vec<u8>,
        signature: Vec<u8>,
        public: &ed25519_dalek::VerifyingKey,
    ) -> Result<Self> {
        let version = &bundle.inventory.product_version;
        let installer_path = format!("/{}", installer_name(version));
        let portable_path = format!("/{}", portable_name(version));
        let url = |path: &str| format!("https://api.github.com{path}");
        let signature_text =
            std::str::from_utf8(&signature).context("manifest signature is not UTF-8")?;
        let verified = manifest::verify_against_files(
            &manifest,
            signature_text,
            public,
            version,
            &bundle.require(FileRole::Installer)?,
            &bundle.require(FileRole::Portable)?,
        )?;
        let expected = manifest::for_bundle(bundle, &url(&installer_path), &url(&portable_path))?;
        ensure!(
            verified == expected,
            "signed update manifest does not name the local candidate feed URLs"
        );
        let read_bound = |role: FileRole| -> Result<PathBuf> {
            let entry = bundle
                .inventory
                .file(role)
                .ok_or_else(|| anyhow!("bundle lacks {role:?}"))?;
            let path = bundle.path_of(entry);
            let (hash, size) = sha256_file(&path)?;
            ensure!(
                size == entry.size && hash == entry.sha256,
                "candidate {role:?} changed after bundle verification"
            );
            Ok(path)
        };
        let installer = read_bound(FileRole::Installer)?;
        let portable = read_bound(FileRole::Portable)?;
        let assets = [
            (
                manifest::MANIFEST_NAME.to_string(),
                "/update-manifest.json".to_string(),
            ),
            (
                manifest::SIGNATURE_NAME.to_string(),
                "/update-manifest.json.sig".to_string(),
            ),
            (installer_name(version), installer_path.clone()),
            (portable_name(version), portable_path.clone()),
        ];
        let listing = serde_json::to_vec(&json!([{
            "tag_name": format!("v{version}"), "draft": false, "prerelease": false,
            "html_url": format!("https://api.github.com/releases/tag/v{version}"),
            "body": format!("ExoSnap {version}"),
            "assets": assets.iter().map(|(name, path)| json!({"name": name, "browser_download_url": url(path)})).collect::<Vec<_>>()
        }]))?;
        let installer_entry = bundle.inventory.file(FileRole::Installer).unwrap();
        let portable_entry = bundle.inventory.file(FileRole::Portable).unwrap();
        Ok(Self {
            listing,
            manifest,
            signature,
            installer,
            portable,
            installer_path,
            portable_path,
            installer_hash: installer_entry.sha256.clone(),
            portable_hash: portable_entry.sha256.clone(),
            installer_size: installer_entry.size,
            portable_size: portable_entry.size,
        })
    }

    fn response(&self, method: &str, path: &str) -> Option<Response> {
        if method != "GET" {
            return None;
        }
        let (content_type, body) = match path.split('?').next()? {
            "/repos/Exoridus/exosnap/releases" => {
                ("application/json", Body::Bytes(self.listing.clone()))
            }
            "/update-manifest.json" => ("application/json", Body::Bytes(self.manifest.clone())),
            "/update-manifest.json.sig" => ("text/plain", Body::Bytes(self.signature.clone())),
            p if p == self.installer_path => (
                "application/octet-stream",
                Body::File(
                    self.installer.clone(),
                    self.installer_hash.clone(),
                    self.installer_size,
                ),
            ),
            p if p == self.portable_path => (
                "application/zip",
                Body::File(
                    self.portable.clone(),
                    self.portable_hash.clone(),
                    self.portable_size,
                ),
            ),
            _ => return None,
        };
        Some(Response { content_type, body })
    }
}

pub fn serve_forever(args: FeedArgs) -> Result<()> {
    if args.generate_cert {
        let der_path = args
            .cert_der
            .as_ref()
            .ok_or_else(|| anyhow!("--cert-der is required with --generate-cert"))?;
        let mut authority = rcgen::CertificateParams::new(Vec::<String>::new())?;
        authority.is_ca = rcgen::IsCa::Ca(rcgen::BasicConstraints::Unconstrained);
        authority.key_usages = vec![
            rcgen::KeyUsagePurpose::KeyCertSign,
            rcgen::KeyUsagePurpose::CrlSign,
        ];
        authority
            .distinguished_name
            .push(rcgen::DnType::CommonName, "ExoSnap disposable update CA");
        let authority_key = rcgen::KeyPair::generate()?;
        let authority_cert = authority.self_signed(&authority_key)?;
        let mut server = rcgen::CertificateParams::new(vec!["api.github.com".to_string()])?;
        server.extended_key_usages = vec![rcgen::ExtendedKeyUsagePurpose::ServerAuth];
        let server_key = rcgen::KeyPair::generate()?;
        let server_cert = server.signed_by(&server_key, &authority_cert, &authority_key)?;
        std::fs::write(
            &args.cert,
            format!("{}{}", server_cert.pem(), authority_cert.pem()),
        )?;
        std::fs::write(&args.key, server_key.serialize_pem())?;
        std::fs::write(der_path, authority_cert.der())?;
        return Ok(());
    }
    let bundle_path = args
        .bundle
        .as_ref()
        .ok_or_else(|| anyhow!("--bundle is required to serve"))?;
    let public_key = args
        .public_key_hex
        .as_ref()
        .ok_or_else(|| anyhow!("--public-key-hex is required to serve"))?;
    let bundle = crate::bundle::open_any(bundle_path)?;
    let public_key = manifest::public_key_from_hex(public_key)?;
    let manifest_path = args
        .manifest
        .as_ref()
        .ok_or_else(|| anyhow!("--manifest is required to serve"))?;
    let signature_path = args
        .signature
        .as_ref()
        .ok_or_else(|| anyhow!("--signature is required to serve"))?;
    let feed = Feed::from_signed(
        &bundle,
        std::fs::read(manifest_path)?,
        std::fs::read(signature_path)?,
        &public_key,
    )?;
    let mut cert_reader = BufReader::new(std::fs::File::open(&args.cert)?);
    let certs = rustls_pemfile::certs(&mut cert_reader).collect::<std::io::Result<Vec<_>>>()?;
    ensure!(
        !certs.is_empty(),
        "TLS certificate file contains no certificates"
    );
    let mut key_reader = BufReader::new(std::fs::File::open(&args.key)?);
    let key = rustls_pemfile::private_key(&mut key_reader)?
        .ok_or_else(|| anyhow!("TLS key file contains no private key"))?;
    let config = Arc::new(
        ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(certs, key)?,
    );
    let listener =
        TcpListener::bind(&args.listen).with_context(|| format!("bind {}", args.listen))?;
    println!(
        "candidate {} HTTPS feed listening on {}",
        bundle.sha256, args.listen
    );
    for incoming in listener.incoming() {
        let socket = incoming?;
        socket.set_read_timeout(Some(std::time::Duration::from_secs(15)))?;
        socket.set_write_timeout(Some(std::time::Duration::from_secs(60)))?;
        let connection = ServerConnection::new(config.clone())?;
        let mut stream = StreamOwned::new(connection, socket);
        if let Err(error) = serve_one(&mut stream, &feed) {
            eprintln!("feed request failed: {error:#}");
        }
    }
    Ok(())
}

fn serve_one(stream: &mut impl ReadWrite, feed: &Feed) -> Result<()> {
    let mut request = Vec::new();
    let mut byte = [0u8; 1];
    while request.len() < 8192 && !request.ends_with(b"\r\n\r\n") {
        if stream.read(&mut byte)? == 0 {
            return Ok(());
        }
        request.push(byte[0]);
    }
    ensure!(
        request.ends_with(b"\r\n\r\n"),
        "HTTP request header too long"
    );
    let request = std::str::from_utf8(&request)?;
    let line = request.lines().next().unwrap_or_default();
    let mut parts = line.split_whitespace();
    let response = match (parts.next(), parts.next()) {
        (Some(method), Some(path)) => feed.response(method, path),
        _ => None,
    };
    let (status, content_type, body) = match response {
        Some(response) => ("200 OK", response.content_type, response.body),
        None => (
            "404 Not Found",
            "text/plain",
            Body::Bytes(b"not found".to_vec()),
        ),
    };
    let (len, mut source): (u64, Box<dyn Read>) = match body {
        Body::Bytes(bytes) => (bytes.len() as u64, Box::new(std::io::Cursor::new(bytes))),
        Body::File(path, expected_hash, expected_size) => {
            let mut options = std::fs::OpenOptions::new();
            options.read(true);
            #[cfg(windows)]
            {
                use std::os::windows::fs::OpenOptionsExt;
                options.share_mode(1);
            }
            let mut file = options.open(path)?;
            let len = file.metadata()?.len();
            ensure!(
                len == expected_size,
                "candidate package size changed before serving"
            );
            use sha2::{Digest, Sha256};
            let mut hash = Sha256::new();
            let mut buffer = [0u8; 65536];
            loop {
                let read = file.read(&mut buffer)?;
                if read == 0 {
                    break;
                }
                hash.update(&buffer[..read]);
            }
            ensure!(
                hex::encode(hash.finalize()) == expected_hash,
                "candidate package hash changed before serving"
            );
            file.rewind()?;
            (len, Box::new(file))
        }
    };
    write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {len}\r\nConnection: close\r\n\r\n"
    )?;
    std::io::copy(&mut source, stream)?;
    stream.flush()?;
    Ok(())
}

trait ReadWrite: Read + Write {}
impl<T: Read + Write> ReadWrite for T {}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bundle::{FileRole, sha256_file};
    use ed25519_dalek::SigningKey;

    #[test]
    fn feed_describes_only_exact_candidate_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = crate::bundle::tests::fixture(dir.path(), "0.10.0");
        let key = SigningKey::from_bytes(&[7; 32]);
        let feed = Feed::new(&bundle, &key).unwrap();
        let listing = feed
            .response("GET", "/repos/Exoridus/exosnap/releases")
            .unwrap();
        let release: serde_json::Value = serde_json::from_slice(&listing.bytes().unwrap()).unwrap();
        assert_eq!(release[0]["tag_name"], "v0.10.0");
        assert_eq!(release[0]["prerelease"], false);
        for (path, role) in [
            ("/ExoSnap-0.10.0-windows-x64.msi", FileRole::Installer),
            (
                "/ExoSnap-0.10.0-windows-x64-portable.zip",
                FileRole::Portable,
            ),
        ] {
            let body = feed.response("GET", path).unwrap().bytes().unwrap();
            assert_eq!(
                crate::bundle::sha256_bytes(&body),
                sha256_file(&bundle.require(role).unwrap()).unwrap().0
            );
        }
        let manifest = feed
            .response("GET", "/update-manifest.json")
            .unwrap()
            .bytes()
            .unwrap();
        let signature = String::from_utf8(
            feed.response("GET", "/update-manifest.json.sig")
                .unwrap()
                .bytes()
                .unwrap(),
        )
        .unwrap();
        crate::manifest::verify_against_files(
            &manifest,
            &signature,
            &key.verifying_key(),
            "0.10.0",
            &bundle.require(FileRole::Installer).unwrap(),
            &bundle.require(FileRole::Portable).unwrap(),
        )
        .unwrap();
    }

    #[test]
    fn feed_rejects_unknown_routes_and_methods() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = crate::bundle::tests::fixture(dir.path(), "0.10.0");
        let key = SigningKey::from_bytes(&[7; 32]);
        let feed = Feed::new(&bundle, &key).unwrap();
        assert!(
            feed.response("POST", "/repos/Exoridus/exosnap/releases")
                .is_none()
        );
        assert!(feed.response("GET", "/other").is_none());
    }

    #[test]
    fn static_signed_feed_rejects_wrong_url_and_signature() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = crate::bundle::tests::fixture(dir.path(), "0.10.0");
        let key = SigningKey::from_bytes(&[7; 32]);
        let doc = crate::manifest::for_bundle(
            &bundle,
            "https://other.example/i.msi",
            "https://other.example/p.zip",
        )
        .unwrap();
        let bytes = crate::manifest::serialize(&doc).unwrap();
        let signature = crate::manifest::sign(&bytes, &key).into_bytes();
        assert!(
            Feed::from_signed(
                &bundle,
                bytes.clone(),
                signature.clone(),
                &key.verifying_key()
            )
            .is_err()
        );
        let mut tampered = bytes;
        tampered.push(b' ');
        assert!(Feed::from_signed(&bundle, tampered, signature, &key.verifying_key()).is_err());
    }

    #[test]
    fn generated_certificate_has_pem_key_and_importable_der() {
        let dir = tempfile::tempdir().unwrap();
        let cert = dir.path().join("feed.pem");
        let key = dir.path().join("feed-key.pem");
        let der = dir.path().join("feed.der");
        serve_forever(FeedArgs {
            bundle: None,
            manifest: None,
            signature: None,
            cert: cert.clone(),
            key: key.clone(),
            public_key_hex: None,
            generate_cert: true,
            cert_der: Some(der.clone()),
            listen: "127.0.0.1:0".into(),
        })
        .unwrap();
        let cert_bytes =
            rustls_pemfile::certs(&mut BufReader::new(std::fs::File::open(cert).unwrap()))
                .collect::<std::io::Result<Vec<_>>>()
                .unwrap();
        let private =
            rustls_pemfile::private_key(&mut BufReader::new(std::fs::File::open(key).unwrap()))
                .unwrap();
        assert_eq!(cert_bytes.len(), 2);
        assert_eq!(cert_bytes[1].as_ref(), std::fs::read(der).unwrap());
        assert!(private.is_some());
    }
}
