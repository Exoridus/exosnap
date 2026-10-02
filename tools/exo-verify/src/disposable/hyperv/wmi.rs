//! Temporary VM lifecycle through the Hyper-V WMI v2 provider
//! (`root\virtualization\v2`).
//!
//! Settings objects travel as embedded instances in CIM-XML (DTD 2.0) text,
//! which is the form `Msvm_VirtualSystemManagementService` requires. Methods
//! that return 4096 started an asynchronous job, which is awaited here so
//! that every call returns only once its effect exists.

use anyhow::{Context as _, Result, bail};
use std::time::{Duration, Instant};
use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance};
use windows::Win32::System::Wmi::{
    IWbemObjectTextSrc, WMI_OBJ_TEXT_CIM_DTD_2_0, WbemObjectTextSrc,
};
use wmi::{IWbemClassWrapper, Variant, WMIConnection};

const NAMESPACE: &str = r"ROOT\virtualization\v2";
const JOB_STARTED: u32 = 4096;
const JOB_COMPLETED: u16 = 7;
const STATE_ENABLED: u16 = 2;
const STATE_DISABLED: u16 = 3;

pub struct Hypervisor {
    wmi: WMIConnection,
    service: String,
}

/// A VM this process defined. Its settings path is needed to attach devices.
pub struct Vm {
    pub system_path: String,
    pub settings_path: String,
    /// The VM id, which is also its Hyper-V socket address.
    pub id: String,
}

pub struct VmSpec<'a> {
    pub name: &'a str,
    pub memory_mb: u64,
    pub processors: u64,
    pub vhdx: &'a str,
}

fn quote(text: &str) -> String {
    text.replace('\\', "\\\\").replace('\'', "\\'")
}

fn string(v: &Variant) -> Option<String> {
    match v {
        Variant::String(s) => Some(s.clone()),
        _ => None,
    }
}

fn number(v: &Variant) -> Option<u64> {
    match *v {
        Variant::UI1(n) => Some(n as u64),
        Variant::UI2(n) => Some(n as u64),
        Variant::UI4(n) => Some(n as u64),
        Variant::UI8(n) => Some(n),
        Variant::I2(n) => u64::try_from(n).ok(),
        Variant::I4(n) => u64::try_from(n).ok(),
        Variant::I8(n) => u64::try_from(n).ok(),
        _ => None,
    }
}

fn objects(wmi: &WMIConnection, wql: &str) -> Result<Vec<IWbemClassWrapper>> {
    Ok(wmi.exec_query(wql)?.collect::<Result<Vec<_>, _>>()?)
}

fn embedded_text(object: &IWbemClassWrapper) -> Result<String> {
    let source: IWbemObjectTextSrc =
        unsafe { CoCreateInstance(&WbemObjectTextSrc, None, CLSCTX_INPROC_SERVER) }
            .context("create WbemObjectTextSrc")?;
    let text = unsafe { source.GetText(0, &object.inner, WMI_OBJ_TEXT_CIM_DTD_2_0.0 as u32, None) }
        .context("serialise settings object")?;
    Ok(text.to_string())
}

impl Hypervisor {
    pub fn connect() -> Result<Hypervisor> {
        let wmi = WMIConnection::with_namespace_path(NAMESPACE)
            .context("connect to the Hyper-V WMI provider")?;
        let service = objects(&wmi, "SELECT * FROM Msvm_VirtualSystemManagementService")?
            .into_iter()
            .next()
            .context("no Msvm_VirtualSystemManagementService")?
            .path()?;
        Ok(Hypervisor { wmi, service })
    }

    fn query_one(&self, wql: &str) -> Result<IWbemClassWrapper> {
        objects(&self.wmi, wql)?
            .into_iter()
            .next()
            .with_context(|| format!("no result for {wql}"))
    }

    /// The host's default settings template for one resource subtype.
    fn default_resource(&self, class: &str, subtype: &str) -> Result<IWbemClassWrapper> {
        let template = objects(
            &self.wmi,
            &format!(
                "SELECT * FROM {class} WHERE ResourceSubType = '{}'",
                quote(subtype)
            ),
        )?
        .into_iter()
        .find(|r| {
            r.get_property("InstanceID")
                .ok()
                .and_then(|v| string(&v))
                .is_some_and(|id| id.ends_with("Default"))
        })
        .with_context(|| format!("no default {subtype} template"))?;
        Ok(template)
    }

    fn call(
        &self,
        object_path: &str,
        method: &str,
        class: &str,
        params: &[(&str, Variant)],
    ) -> Result<IWbemClassWrapper> {
        let in_params = self
            .wmi
            .get_object(class)?
            .get_method(method)?
            .with_context(|| format!("{class}.{method} has no parameters"))?
            .spawn_instance()?;
        for (name, value) in params {
            in_params.put_property(name, value.clone())?;
        }
        let out = self
            .wmi
            .exec_method(object_path, method, Some(&in_params))?
            .with_context(|| format!("{method} returned nothing"))?;
        let code = number(&out.get_property("ReturnValue")?).unwrap_or(u64::MAX) as u32;
        match code {
            0 => {}
            JOB_STARTED => {
                let job = string(&out.get_property("Job")?).context("job reference")?;
                self.await_job(&job, method)?;
            }
            other => bail!("{method} failed with {other}"),
        }
        Ok(out)
    }

    fn await_job(&self, job: &str, method: &str) -> Result<()> {
        let deadline = Instant::now() + Duration::from_secs(600);
        loop {
            let state = self.wmi.get_object(job)?;
            let value = number(&state.get_property("JobState")?).unwrap_or(0) as u16;
            match value {
                JOB_COMPLETED => return Ok(()),
                3..=5 => {}
                _ => {
                    let detail = state
                        .get_property("ErrorDescription")
                        .ok()
                        .and_then(|v| string(&v))
                        .unwrap_or_default();
                    bail!("{method} job ended in state {value}: {detail}");
                }
            }
            if Instant::now() >= deadline {
                bail!("{method} job did not finish within 600 s");
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    }

    fn add_resource(&self, settings_path: &str, resource: &IWbemClassWrapper) -> Result<String> {
        let out = self.call(
            &self.service,
            "AddResourceSettings",
            "Msvm_VirtualSystemManagementService",
            &[
                (
                    "AffectedConfiguration",
                    Variant::String(settings_path.into()),
                ),
                (
                    "ResourceSettings",
                    Variant::Array(vec![Variant::String(embedded_text(resource)?)]),
                ),
            ],
        )?;
        match out.get_property("ResultingResourceSettings")? {
            Variant::Array(items) => items.first().and_then(string),
            other => string(&other),
        }
        .context("AddResourceSettings returned no resource")
    }

    fn modify_resource(&self, resource: &IWbemClassWrapper) -> Result<()> {
        self.call(
            &self.service,
            "ModifyResourceSettings",
            "Msvm_VirtualSystemManagementService",
            &[(
                "ResourceSettings",
                Variant::Array(vec![Variant::String(embedded_text(resource)?)]),
            )],
        )?;
        Ok(())
    }

    /// Defines a generation 2 VM that boots `spec.vhdx`, without checkpoints
    /// and without networking.
    pub fn define(&self, spec: &VmSpec) -> Result<Vm> {
        let settings = self
            .wmi
            .get_object("Msvm_VirtualSystemSettingData")?
            .spawn_instance()?;
        settings.put_property("ElementName", spec.name.to_string())?;
        settings.put_property(
            "VirtualSystemSubType",
            "Microsoft:Hyper-V:SubType:2".to_string(),
        )?;
        // An automatic checkpoint would outlive the disposable disk.
        settings.put_property("AutomaticSnapshotsEnabled", false)?;
        let out = self.call(
            &self.service,
            "DefineSystem",
            "Msvm_VirtualSystemManagementService",
            &[("SystemSettings", Variant::String(embedded_text(&settings)?))],
        )?;
        let system_path =
            string(&out.get_property("ResultingSystem")?).context("DefineSystem returned no VM")?;
        let system = self.wmi.get_object(&system_path)?;
        let id = string(&system.get_property("Name")?).context("VM without id")?;
        let settings_path = self
            .query_one(&format!(
                "SELECT * FROM Msvm_VirtualSystemSettingData WHERE VirtualSystemIdentifier = '{}' AND VirtualSystemType = 'Microsoft:Hyper-V:System:Realized'",
                quote(&id)
            ))?
            .path()?;
        let vm = Vm {
            system_path,
            settings_path,
            id,
        };
        if let Err(e) = self.configure(&vm, spec) {
            let _ = self.destroy(&vm);
            return Err(e);
        }
        Ok(vm)
    }

    fn configure(&self, vm: &Vm, spec: &VmSpec) -> Result<()> {
        let id = quote(&vm.id);
        let memory = self.query_one(&format!(
            "SELECT * FROM Msvm_MemorySettingData WHERE InstanceID LIKE 'Microsoft:{id}%'"
        ))?;
        memory.put_property("VirtualQuantity", spec.memory_mb)?;
        memory.put_property("Reservation", spec.memory_mb)?;
        memory.put_property("Limit", spec.memory_mb)?;
        memory.put_property("DynamicMemoryEnabled", false)?;
        self.modify_resource(&memory)?;
        let processor = self.query_one(&format!(
            "SELECT * FROM Msvm_ProcessorSettingData WHERE InstanceID LIKE 'Microsoft:{id}%'"
        ))?;
        processor.put_property("VirtualQuantity", spec.processors)?;
        self.modify_resource(&processor)?;

        let controller = self.default_resource(
            "Msvm_ResourceAllocationSettingData",
            "Microsoft:Hyper-V:Synthetic SCSI Controller",
        )?;
        let controller = self.add_resource(&vm.settings_path, &controller)?;
        let drive = self.default_resource(
            "Msvm_ResourceAllocationSettingData",
            "Microsoft:Hyper-V:Synthetic Disk Drive",
        )?;
        drive.put_property("Parent", controller)?;
        drive.put_property("AddressOnParent", "0".to_string())?;
        let drive = self.add_resource(&vm.settings_path, &drive)?;
        let disk = self.default_resource(
            "Msvm_StorageAllocationSettingData",
            "Microsoft:Hyper-V:Virtual Hard Disk",
        )?;
        disk.put_property("Parent", drive)?;
        disk.put_property(
            "HostResource",
            Variant::Array(vec![Variant::String(spec.vhdx.to_string())]),
        )?;
        self.add_resource(&vm.settings_path, &disk)?;
        Ok(())
    }

    pub fn state(&self, vm: &Vm) -> Result<u16> {
        let system = self.wmi.get_object(&vm.system_path)?;
        Ok(number(&system.get_property("EnabledState")?).unwrap_or(0) as u16)
    }

    fn request_state(&self, vm: &Vm, state: u16) -> Result<()> {
        self.call(
            &vm.system_path,
            "RequestStateChange",
            "Msvm_ComputerSystem",
            &[("RequestedState", Variant::UI2(state))],
        )?;
        Ok(())
    }

    pub fn start(&self, vm: &Vm) -> Result<()> {
        self.request_state(vm, STATE_ENABLED)
    }

    /// Turns the VM off without a guest shutdown. The disk is discarded
    /// afterwards, so nothing in the guest needs to be flushed.
    pub fn power_off(&self, vm: &Vm) -> Result<()> {
        if self.state(vm)? == STATE_DISABLED {
            return Ok(());
        }
        self.request_state(vm, STATE_DISABLED)
    }

    pub fn is_off(&self, vm: &Vm) -> Result<bool> {
        Ok(self.state(vm)? == STATE_DISABLED)
    }

    pub fn destroy(&self, vm: &Vm) -> Result<()> {
        let _ = self.power_off(vm);
        self.call(
            &self.service,
            "DestroySystem",
            "Msvm_VirtualSystemManagementService",
            &[("AffectedSystem", Variant::String(vm.system_path.clone()))],
        )?;
        Ok(())
    }
}

/// Whether this host can manage Hyper-V VMs at all.
pub fn available() -> bool {
    Hypervisor::connect().is_ok()
}
