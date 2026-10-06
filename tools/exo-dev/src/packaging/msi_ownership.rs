use super::ValidationReport;

#[cfg(test)]
#[path = "msi_ownership_tests.rs"]
mod native_tests;

pub(super) fn validate(text: &str, report: &mut ValidationReport) {
    let doc = match roxmltree::Document::parse(text) {
        Ok(doc) => doc,
        Err(error) => {
            report
                .errors
                .push(format!("packaging/msi/Package.wxs: invalid XML: {error}"));
            return;
        }
    };
    let find = |tag: &str, attribute: &str, value: &str| {
        let mut matches = doc
            .descendants()
            .filter(|node| node.has_tag_name(tag) && node.attribute(attribute) == Some(value));
        let first = matches.next();
        if matches.next().is_some() {
            None
        } else {
            first
        }
    };
    let mut require = |valid: bool, message: &str| {
        if !valid {
            report
                .errors
                .push(format!("packaging/msi/Package.wxs: {message}"));
        }
    };
    for id in [
        "EXOSNAP_DISTRIBUTION_OWNER",
        "EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED",
        "EXOSNAP_DISTRIBUTION_OWNER_PRESENT",
        "EXOSNAP_DISTRIBUTION_OWNER_STRING",
        "EXOSNAP_RESOLVED_DISTRIBUTION_OWNER",
    ] {
        require(
            find("Property", "Id", id).is_some_and(|node| {
                node.attribute("Secure") == Some("yes") && node.attribute("Value").is_none()
            }),
            "distribution ownership properties must be secure with no premature default",
        );
    }
    require(
        find("RegistrySearch", "Id", "ExoSnapDistributionOwnerRemembered").is_some_and(|node| {
            node.attribute("Root") == Some("HKLM")
                && node.attribute("Key") == Some("Software\\ExoSnap")
                && node.attribute("Name") == Some("DistributionOwner")
                && node.attribute("Type") == Some("raw")
                && node.attribute("Bitness") == Some("always64")
                && node.parent().and_then(|parent| parent.attribute("Id"))
                    == Some("EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED")
        }),
        "distribution owner AppSearch must read the authoritative 64-bit HKLM product marker",
    );
    let caller_condition = "NOT EXOSNAP_DISTRIBUTION_OWNER OR EXOSNAP_DISTRIBUTION_OWNER = \"direct\" OR EXOSNAP_DISTRIBUTION_OWNER = \"winget\" OR EXOSNAP_DISTRIBUTION_OWNER = \"chocolatey\"";
    require(
        doc.descendants().any(|node| {
            node.has_tag_name("Launch") && node.attribute("Condition") == Some(caller_condition)
        }),
        "distribution owner caller must reject values outside direct, winget and chocolatey",
    );
    require(
        find("RegistryValue", "Name", "DistributionOwner").is_some_and(|owner| {
            owner.attribute("Root") == Some("HKLM")
                && owner.attribute("Key") == Some("Software\\ExoSnap")
                && owner.attribute("Type") == Some("string")
                && owner.attribute("Value") == Some("[EXOSNAP_RESOLVED_DISTRIBUTION_OWNER]")
                && owner.parent().is_some_and(|component| {
                    component.has_tag_name("Component")
                        && ["installed", "InstallPath"].iter().all(|name| {
                            component.children().any(|node| {
                                node.has_tag_name("RegistryValue")
                                    && node.attribute("Name") == Some(name)
                                    && node.attribute("Root") == Some("HKLM")
                                    && node.attribute("Key") == Some("Software\\ExoSnap")
                            })
                        })
                })
        }),
        "distribution owner must be persisted with installed and InstallPath in the same HKLM product component",
    );
    require(
        find("Binary", "Id", "DistributionOwnerProbe").is_some_and(|node| {
            node.attribute("SourceFile") == Some("$(var.DistributionOwnerProbePath)")
        }) && find("CustomAction", "Id", "ProbeDistributionOwner").is_some_and(|node| {
            node.attribute("BinaryRef") == Some("DistributionOwnerProbe")
                && node.attribute("DllEntry") == Some("ProbeDistributionOwner")
                && node.attribute("Execute") == Some("immediate")
                && node.attribute("Return") == Some("check")
        }),
        "distribution owner presence requires the embedded read-only native probe",
    );
    for sequence in ["InstallUISequence", "InstallExecuteSequence"] {
        require(
            doc.descendants()
                .filter(|node| node.has_tag_name(sequence))
                .flat_map(|node| node.children())
                .any(|node| {
                    node.has_tag_name("Custom")
                        && node.attribute("Action") == Some("ProbeDistributionOwner")
                        && node.attribute("After") == Some("AppSearch")
                        && node.attribute("Before").is_none()
                        && node.attribute("Condition").is_none()
                }),
            "distribution owner presence must be checked after AppSearch in both sequences before resolving ownership",
        );
    }
    let actions = [
        (
            "ResolveDistributionOwnerFromCaller",
            "[EXOSNAP_DISTRIBUTION_OWNER]",
            "ProbeDistributionOwner",
            "EXOSNAP_DISTRIBUTION_OWNER",
        ),
        (
            "ResolveDistributionOwnerFromRegistry",
            "[EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED]",
            "ResolveDistributionOwnerFromCaller",
            "NOT EXOSNAP_DISTRIBUTION_OWNER AND EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED AND EXOSNAP_DISTRIBUTION_OWNER_STRING",
        ),
        (
            "ResolveDistributionOwnerUnknown",
            "unknown",
            "ResolveDistributionOwnerFromRegistry",
            "NOT EXOSNAP_DISTRIBUTION_OWNER AND EXOSNAP_DISTRIBUTION_OWNER_PRESENT AND (NOT EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED OR NOT EXOSNAP_DISTRIBUTION_OWNER_STRING)",
        ),
        (
            "ResolveDistributionOwnerDefault",
            "direct",
            "ResolveDistributionOwnerUnknown",
            "NOT EXOSNAP_DISTRIBUTION_OWNER AND NOT EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED AND NOT EXOSNAP_DISTRIBUTION_OWNER_PRESENT",
        ),
    ];
    for (id, value, after, condition) in actions {
        require(
            find("CustomAction", "Id", id).is_some_and(|node| {
                node.attribute("Property") == Some("EXOSNAP_RESOLVED_DISTRIBUTION_OWNER")
                    && node.attribute("Value") == Some(value)
            }),
            "distribution owner must resolve caller, persisted marker, then direct with property actions",
        );
        for sequence in ["InstallUISequence", "InstallExecuteSequence"] {
            require(
                doc.descendants()
                    .filter(|node| node.has_tag_name(sequence))
                    .flat_map(|node| node.children())
                    .any(|node| {
                        node.has_tag_name("Custom")
                            && node.attribute("Action") == Some(id)
                            && node.attribute("After") == Some(after)
                            && node.attribute("Before").is_none()
                            && node.attribute("Condition") == Some(condition)
                    }),
                "distribution ownership must resolve after AppSearch in both UI and execute sequences before product removal",
            );
        }
    }
    require(
        doc.descendants().any(|node| {
            node.has_tag_name("MajorUpgrade")
                && matches!(
                    node.attribute("Schedule"),
                    None | Some("afterInstallValidate")
                )
        }),
        "distribution ownership resolution requires product removal after launch validation",
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    const PACKAGE: &str = include_str!("../../../../packaging/msi/Package.wxs");

    #[test]
    fn existing_empty_owner_requires_a_native_presence_probe() {
        let doc = roxmltree::Document::parse(PACKAGE).unwrap();
        assert!(doc.descendants().any(|node| node.has_tag_name("Binary")
            && node.attribute("Id") == Some("DistributionOwnerProbe")
            && node.attribute("SourceFile") == Some("$(var.DistributionOwnerProbePath)")));
        assert!(
            doc.descendants()
                .any(|node| node.has_tag_name("CustomAction")
                    && node.attribute("Id") == Some("ProbeDistributionOwner")
                    && node.attribute("BinaryRef") == Some("DistributionOwnerProbe")
                    && node.attribute("DllEntry") == Some("ProbeDistributionOwner")
                    && node.attribute("Execute") == Some("immediate"))
        );
        assert!(
            doc.descendants()
                .any(|node| node.has_tag_name("CustomAction")
                    && node.attribute("Id") == Some("ResolveDistributionOwnerUnknown")
                    && node.attribute("Value") == Some("unknown"))
        );
    }
    #[test]
    fn canonical_msi_ownership_schema_is_present() {
        let mut report = ValidationReport::default();
        validate(PACKAGE, &mut report);
        let doc = roxmltree::Document::parse(PACKAGE).unwrap();
        assert!(
            doc.descendants()
                .any(|node| node.attribute("Id") == Some("EXOSNAP_DISTRIBUTION_OWNER")),
            "missing secure distribution caller property"
        );
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn ownership_schema_rejects_missing_defaults_and_non_authoritative_markers() {
        for (from, to) in [
            (
                "Id=\"EXOSNAP_DISTRIBUTION_OWNER\" Secure=\"yes\"",
                "Id=\"EXOSNAP_DISTRIBUTION_OWNER\" Value=\"direct\" Secure=\"yes\"",
            ),
            (
                "Id=\"EXOSNAP_DISTRIBUTION_OWNER\" Secure=\"yes\"",
                "Id=\"EXOSNAP_DISTRIBUTION_OWNER\"",
            ),
            (
                "DllEntry=\"ProbeDistributionOwner\"",
                "DllEntry=\"OtherProbe\"",
            ),
            (
                "After=\"AppSearch\" />",
                "After=\"RemoveExistingProducts\" />",
            ),
            ("Value=\"unknown\"", "Value=\"direct\""),
            ("AND EXOSNAP_DISTRIBUTION_OWNER_STRING", ""),
            (
                "AND NOT EXOSNAP_DISTRIBUTION_OWNER_PRESENT",
                "AND EXOSNAP_DISTRIBUTION_OWNER_PRESENT",
            ),
            ("Bitness=\"always64\"", "Bitness=\"always32\""),
            ("Name=\"DistributionOwner\"", "Name=\"OtherOwner\""),
            (
                "Value=\"[EXOSNAP_RESOLVED_DISTRIBUTION_OWNER]\"",
                "Value=\"direct\"",
            ),
            (
                "EXOSNAP_DISTRIBUTION_OWNER = \"direct\"",
                "EXOSNAP_DISTRIBUTION_OWNER ~= \"direct\"",
            ),
            (
                "After=\"ProbeDistributionOwner\" Condition=\"EXOSNAP_DISTRIBUTION_OWNER\"",
                "After=\"RemoveExistingProducts\" Condition=\"EXOSNAP_DISTRIBUTION_OWNER\"",
            ),
            (
                "Value=\"[EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED]\"",
                "Value=\"direct\"",
            ),
            (
                "NOT EXOSNAP_DISTRIBUTION_OWNER AND NOT EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED",
                "NOT EXOSNAP_DISTRIBUTION_OWNER",
            ),
        ] {
            assert!(
                PACKAGE.contains(from),
                "fixture does not carry mutation target {from}"
            );
            let mut report = ValidationReport::default();
            validate(&PACKAGE.replace(from, to), &mut report);
            assert!(!report.ok(), "mutation escaped: {from} -> {to}");
        }
        let marker_start = PACKAGE.find("        <RegistryValue Root=\"HKLM\"\n                       Key=\"Software\\ExoSnap\"\n                       Name=\"DistributionOwner\"").unwrap();
        let marker_end = marker_start + PACKAGE[marker_start..].find("/>").unwrap() + 2;
        let marker = &PACKAGE[marker_start..marker_end];
        let moved = PACKAGE.replace(marker, "").replace("    <Component Id=\"LegacyHandoff\"", &format!("    <Component Id=\"WrongOwner\" Directory=\"INSTALLFOLDER\">{marker}</Component>\n    <Component Id=\"LegacyHandoff\""));
        let mut report = ValidationReport::default();
        validate(&moved, &mut report);
        assert!(
            !report.ok(),
            "ownership moved outside authoritative product component"
        );
    }
}
