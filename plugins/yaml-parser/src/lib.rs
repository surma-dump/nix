use std::{
    alloc::{Layout, alloc},
    ffi::{CStr, CString},
};

#[unsafe(no_mangle)]
fn parse_yaml(ptr: *const i8) -> u32 {
    let s = unsafe { CStr::from_ptr(ptr) };
    let Ok(v) = _parse_yaml(s) else {
        return 0;
    };
    let s = Box::leak(v.into_boxed_c_str());
    s.as_ptr() as u32
}

fn _parse_yaml(s: &CStr) -> anyhow::Result<CString> {
    let s = s.to_str()?;
    let v: serde_json::Value = serde_json::from_str(s)?;
    let Some(s) = v.as_str() else {
        anyhow::bail!("Must be a string")
    };
    let v: serde_yml::Value = serde_yml::from_str(s)?;
    let str = serde_json::to_string(&v)?;
    let str = CString::new(str)?;
    Ok(str)
}

#[unsafe(no_mangle)]
pub fn _malloc(size: u32) -> u32 {
    let p = unsafe { alloc(Layout::from_size_align(size as usize, 8).unwrap()) };
    p as u32
}
