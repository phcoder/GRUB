#![no_std]
#![no_main]
#![feature(format_args_nl)]
#![feature(inherent_str_constructors)]

extern crate alloc;
extern crate grub;

use alloc::string::ToString;
use core::cmp::min;
use core::convert::TryFrom;

use alloc::format;
use core::ffi::c_void;

use grub::dprintln;
use grub::println;
use grub::eformat;

#[unsafe(link_section = ".module_license")]
pub static LICENSE: [u8; 15] = *b"LICENSE=GPLv3+\0";


fn rust_hello (argv: &[&str]) -> Result<(), grub::GrubError> {
    println!("Hello, world argv={argv:?}");
    dprintln!("hello", "hello from debug");
    return Ok(());
}

fn rust_err (argv: &[&str]) -> Result<(), grub::GrubError> {
    return Err(eformat!(grub::ErrT::Io, "hello from error argv={argv:?}"));
}

fn hexdump (start: u64, buf: &[u8])
{
    let mut off = 0usize;
    let mut bse = start;

    while off < buf.len() {
	let mut line = "".to_string();
	let cnt = min(buf.len() - off, 16);

	line += &format!("{bse:08x}: ");

	for i in 0..cnt {
	    let c: u8 = buf[i+off];
	    line += &format!("{c:02x} ");
	    if (i & 7) == 7 {
		line += " ";
	    }
	}

	for i in cnt..16 {
	    line += "   ";
	    if (i & 7) == 7 {
		line += " ";
	    }
	}

	line += "|";

	for i in 0..cnt {
	    line += if (buf[i+off] >= 32) && (buf[i+off] < 127) { str::from_utf8(&buf[i+off..(i+off+1)]).unwrap_or(".") } else { "." };
	}

	line += "|";

	println!("{line}");

	/* Print only first and last line if more than 3 lines are identical.  */
	if off + 4 * 16 <= buf.len()
	    && buf[off..(off+16)] == buf[(off+16*1)..(off+16*2)]
	    && buf[off..(off+16)] == buf[(off+16*2)..(off+16*3)]
	    && buf[off..(off+16)] == buf[(off+16*3)..(off+16*4)] {
		println!("*");
		loop {
		    bse += 16;
		    off += 16;
		    if off + 3 * 16 < buf.len() || buf[off..(off+16)] != buf[(off + 2 * 16)..(off + 3 * 16)] {
			break;
		    }
		}
	    }

	off += 16;
	bse += 16;
    }
}


fn rust_hexdump (args: &[&str]) -> Result<(), grub::GrubError> {
    let mut length = 256;
    let mut skip = 0;

    let mut file = grub::File::open(args[0], &grub::FileType::Hexcat)?;

    file.seek(skip);

    loop {
	let mut buf = [0u8; 4096];
	let size = file.read(&mut buf)?;
	if size == 0 {
	    break;
	}
	let len = if length != 0 {min (length, size)} else {size};

	hexdump(skip, &buf[0..len]);
	skip += u64::try_from(len)?;

	if length != 0 {
	    length -= len;
	    if length == 0 {
		break;
	    }
	}
    };
    return Ok(());
}

static HELLO_CMD: grub::ModuleCell<grub::Command> = grub::ModuleCell::new();
static ERR_CMD: grub::ModuleCell<grub::Command> = grub::ModuleCell::new();
static HEXDUMP_CMD: grub::ModuleCell<grub::Command> = grub::ModuleCell::new();
static MODULE: grub::ModuleRefHolder = grub::ModuleRefHolder::new();

#[unsafe(no_mangle)]
pub extern "C" fn grub_mod_init(module: *const c_void) {
    MODULE.init(module);
    HELLO_CMD.set(&MODULE, grub::Command::register("rust_hello", rust_hello,
				      "Rust hello", "Say hello from Rust."));
    ERR_CMD.set(&MODULE, grub::Command::register("rust_err", rust_err,
					"Rust error", "Error out from Rust."));
    HEXDUMP_CMD.set(&MODULE, grub::Command::register("rust_hexdump", rust_hexdump,
					    "Rust hexdump", "Hexdump a file from Rust."));
}

#[unsafe(no_mangle)]
pub extern "C" fn grub_mod_fini() {
    MODULE.fini();
}
