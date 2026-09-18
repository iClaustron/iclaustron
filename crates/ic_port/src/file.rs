// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! File helpers (`ic_open_file`, `ic_create_file`, `ic_mkdir`,
//! `ic_read_file`, `ic_write_file`, `ic_delete_file`,
//! `ic_get_file_contents`). Files are opened read/write with `O_SYNC`
//! as the C code did.

use std::fs::DirBuilder;
use std::fs::File;
use std::fs::OpenOptions;
use std::io::Read;
use std::io::Write;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::fs::OpenOptionsExt;

use crate::debug::FILE_LEVEL;
use crate::IcError;

/// Open an existing file read/write; create it first if `create_flag`.
pub fn open_file(file_name: &str, create_flag: bool) -> Result<File, IcError> {
  let _dbg = crate::debug_entry!("open_file");
  let mut options = OpenOptions::new();
  options
    .read(true)
    .write(true)
    .truncate(false)
    .custom_flags(libc::O_SYNC);
  if create_flag {
    options.create(true).mode(0o600);
  }
  match options.open(file_name) {
    Ok(f) => {
      crate::debug_print!(FILE_LEVEL, "Open file {}", file_name);
      Ok(f)
    }
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// Create a file (truncating any old one) and open it read/write.
pub fn create_file(file_name: &str) -> Result<File, IcError> {
  let _dbg = crate::debug_entry!("create_file");
  let mut options = OpenOptions::new();
  options
    .read(true)
    .write(true)
    .create(true)
    .truncate(true)
    .mode(0o600)
    .custom_flags(libc::O_SYNC);
  match options.open(file_name) {
    Ok(f) => {
      crate::debug_print!(FILE_LEVEL, "Create file {}", file_name);
      Ok(f)
    }
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// Create a directory with mode 0750; an existing directory is fine.
pub fn mkdir(dir_name: &str) -> Result<(), IcError> {
  crate::debug_print!(FILE_LEVEL, "Create dir_name = {}", dir_name);
  let result = DirBuilder::new().mode(0o750).create(dir_name);
  match result {
    Ok(()) => Ok(()),
    Err(e) => {
      if e.kind() == std::io::ErrorKind::AlreadyExists {
        return Ok(());
      }
      let err = IcError::from_io(&e);
      crate::ic_printf!("mkdir failed {}: {}", err.code, err.message());
      Err(err)
    }
  }
}

/// Close a file. In Rust dropping the `File` closes it; this exists for
/// symmetry with the C code and to report the error from the final
/// flush.
pub fn close_file(file: File) -> Result<(), IcError> {
  let _dbg = crate::debug_entry!("close_file");
  match file.sync_all() {
    Ok(()) => Ok(()),
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// Write the whole buffer, retrying partial writes.
pub fn write_file(file: &mut File, buf: &[u8]) -> Result<(), IcError> {
  let _dbg = crate::debug_entry!("write_file");
  crate::debug_print!(FILE_LEVEL, "Write file, size = {}", buf.len());
  match file.write_all(buf) {
    Ok(()) => Ok(()),
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// One read into the buffer; returns the number of bytes read (0 at end
/// of file).
pub fn read_file(file: &mut File, buf: &mut [u8]) -> Result<u64, IcError> {
  let _dbg = crate::debug_entry!("read_file");
  match file.read(buf) {
    Ok(n) => {
      crate::debug_print!(FILE_LEVEL, "Read = {}", n);
      Ok(n as u64)
    }
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// Delete a file; a file that does not exist counts as deleted.
pub fn delete_file(file_name: &str) -> Result<(), IcError> {
  crate::debug_print!(FILE_LEVEL, "Delete file {}", file_name);
  match std::fs::remove_file(file_name) {
    Ok(()) => Ok(()),
    Err(e) => {
      if e.kind() == std::io::ErrorKind::NotFound {
        return Ok(());
      }
      Err(IcError::from_io(&e))
    }
  }
}

/// Read a whole file into memory.
pub fn get_file_contents(file_name: &str) -> Result<Vec<u8>, IcError> {
  let _dbg = crate::debug_entry!("get_file_contents");
  match std::fs::read(file_name) {
    Ok(v) => Ok(v),
    Err(e) => Err(IcError::from_io(&e)),
  }
}

/// True if a file (or directory) exists at the path.
pub fn file_exists(file_name: &str) -> bool {
  std::path::Path::new(file_name).exists()
}

#[cfg(test)]
mod tests {
  use super::*;

  fn temp_name(name: &str) -> String {
    let mut path = std::env::temp_dir();
    path.push(format!("ic_port_{}_{}", std::process::id(), name));
    path.to_string_lossy().to_string()
  }

  #[test]
  fn create_write_read_delete() {
    let name = temp_name("file_test.txt");
    let mut f = create_file(&name).expect("create");
    write_file(&mut f, b"hello file").expect("write");
    close_file(f).expect("close");
    assert!(file_exists(&name));
    let contents = get_file_contents(&name).expect("contents");
    assert_eq!(contents, b"hello file");
    let mut f = open_file(&name, false).expect("open");
    let mut buf = [0u8; 5];
    let n = read_file(&mut f, &mut buf).expect("read");
    assert_eq!(n, 5);
    assert_eq!(&buf, b"hello");
    drop(f);
    delete_file(&name).expect("delete");
    assert!(!file_exists(&name));
    delete_file(&name).expect("delete twice is fine");
  }

  #[test]
  fn mkdir_twice_is_fine() {
    let name = temp_name("dir_test");
    mkdir(&name).expect("mkdir");
    mkdir(&name).expect("mkdir again");
    let _ = std::fs::remove_dir(&name);
  }
}
