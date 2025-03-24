#![no_std]
#![no_main]
#![feature(extern_types)]
#![feature(rustc_attrs)]
#![feature(format_args_nl)]
#![feature(inherent_str_constructors)]
#![warn(static_mut_refs)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec::Vec;
use alloc::vec;
use alloc::string::String;
use alloc::string::ToString;
use alloc::ffi::CString;

use core::alloc::{GlobalAlloc, Layout};
use core::cell::Cell;
use core::cell::RefCell;
use core::ffi::c_char;
use core::ffi::c_int;
use core::ffi::c_void;
use core::ffi::CStr;
use core::fmt::{self, Arguments, Write};
use core::num::TryFromIntError;
use core::panic::PanicInfo;

#[unsafe(link_section = ".module_license")]
pub static LICENSE: [u8; 15] = *b"LICENSE=GPLv3+\0";

unsafe extern "C" {
    static grub_xputs: extern "C" fn(stri: *const c_char);
    fn grub_abort();
    fn grub_memalign(align: usize, sz: usize) -> *mut u8;
    fn grub_free(ptr: *mut u8);
    fn grub_register_command_prio (name: *const c_char,
				   func: extern "C" fn (cmd: *const GrubCommand,
							argc: c_int, argv: *const *const c_char) -> ErrT,
				   summary: *const c_char,
				   description: *const c_char,
				   prio: c_int) -> *mut GrubCommand;
    fn grub_strlen (s: *const c_char) -> usize;
    fn grub_unregister_command (cmd: *const GrubCommand);
    fn grub_refresh ();
    fn grub_debug_enabled(cond: *const c_char) -> bool;
    fn grub_error (n: ErrT, fmt: *const c_char, args: ...) -> ErrT;

    fn grub_file_open (name: *const c_char, typ: FileType) -> *mut GrubFile;
    fn grub_file_read (fil: *mut GrubFile, buf: *mut c_void, len: usize) -> isize;
    fn grub_file_seek (fil: *mut GrubFile, offset: u64) -> u64;
    fn grub_file_close (fil: *mut GrubFile);

    static grub_errno: ErrT;
    static grub_errmsg: *const c_char;
}

#[macro_export]
#[cfg_attr(not(test), rustc_diagnostic_item = "print_macro")]
macro_rules! print {
    ($($arg:tt)*) => {{
        $crate::print_fmt(format_args!($($arg)*));
    }};
}

#[macro_export]
#[cfg_attr(not(test), rustc_diagnostic_item = "println_macro")]
macro_rules! println {
    () => {
        $crate::print!("\n")
    };
    ($($arg:tt)*) => {{
        $crate::print_fmt(format_args_nl!($($arg)*));
    }};
}

#[macro_export]
macro_rules! dprintln {
    ($cond:expr, $($args: expr),*) => {
	$crate::real_dprintln(file!(), line!(), $cond, format_args_nl!($($args)*));
    }
}

#[macro_export]
macro_rules! eformat {
    ($num:expr, $($args: expr),*) => {
	$crate::GrubError::new_fmt($num, format_args!($($args)*))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn strlen(s: *const c_char) -> usize {
    return unsafe { grub_strlen(s) }; 
}


// TODO: Use code generation?
#[repr(C)]
struct GrubCommand {
    next: *mut GrubCommand,
    prev: *mut *mut GrubCommand,
    name: *const c_char,
    prio: c_int,
    func: *const c_void,
    flags: u32,
    summary: *const c_char,
    description: *const c_char,
    data: *const c_void,
}

#[repr(C)]
struct GrubFile {
    /* File name.  */
    name: *const c_char,

    /* The underlying device.  */
    device: *const c_void,

    /* The underlying filesystem.  */
    fs: *const c_void,

    /* The current offset.  */
    offset: u64,
    progress_offset: u64,

    /* Progress info. */
    last_progress_time: u64,
    last_progress_offset: u64,
    estimated_speed: u64,

    /* The file size.  */
    size: u64,

    /* If file is not easily seekable. Should be set by underlying layer.  */
    not_easily_seekable: bool,

    /* Filesystem-specific data.  */
    data: *const c_void,

    /* This is called when a sector is read. Used only for a disk device.  */
    read_hook: *const c_void,

    /* Caller-specific data passed to the read hook.  */
    read_hook_data: *const c_void,
}

// TODO: Use codegen here
#[derive(Clone)]
#[repr(u32)]
pub enum ErrT {
    None = 0,
    TestFailure = 1,
    BadModule = 2,
    OutOfMemory = 3,
    BadFileType = 4,
    FileNotFound = 5,
    FileReadError = 6,
    BadFilename = 7,
    UnknownFs = 8,
    BadFs = 9,
    BadNumber = 10,
    OutOfRange = 11,
    UnknownDevice = 12,
    BadDevice = 13,
    ReadError = 14,
    WriteError = 15,
    UnknownCommand = 16,
    InvalidCommand = 17,
    BadArgument = 18,
    BadPartTable = 19,
    UnknownOs = 20,
    BadOs = 21,
    NoKernel = 22,
    BadFont = 23,
    NotImplementedYet = 24,
    SymlinkLoop = 25,
    BadCompressedData = 26,
    Menu = 27,
    Timeout = 28,
    Io = 29,
    AccessDenied = 30,
    Extractor = 31,
    NetBadAddress = 32,
    NetRouteLoop = 33,
    NetNoRoute = 34,
    NetNoAnswer = 35,
    NetNoCard = 36,
    Wait = 37,
    Bug = 38,
    NetPortClosed = 39,
    NetInvalidResponse = 40,
    NetUnknownError = 41,
    NetPacketTooBig = 42,
    NetNoDomain = 43,
    Eof = 44,
    BadSignature = 45,
    BadFirmware = 46,
    StillReferenced = 47,
    RecursionDepth = 48,
}

#[derive(Clone)]
#[repr(u32)]
pub enum FileType {
    None = 0,
    GrubModule = 1,
    Loopback = 2,
    LinuxKernel = 3,
    LinuxInitrd = 4,

    MultibootKernel = 5,
    MultibootModule = 6,

    XenHypervisor = 7,
    XenModule = 8,

    BsdKernel = 9,
    FreebsdEnv = 10,
    FreebsdModule = 11,
    FreebsdModuleElf = 12,
    NetbsdModule = 13,
    OpenbsdRamdisk = 14,

    XnuInfoPlist = 15,
    XnuMkext = 16,
    XnuKext = 17,
    XnuKernel = 18,
    XnuRamdisk = 19,
    XnuHibernateImage = 20,
    GrubFileXnuDevprop = 21,

    Plan9Kernel = 22,

    Ntldr = 23,
    Truecrypt = 24,
    Freedos = 25,
    Pxechainloader = 26,
    Pcchainloader = 27,

    CorebootChainloader = 28,

    EfiChainloadedImage = 29,

    Signature = 30,
    PublicKey = 31,
    PublicKeyTrust = 32,
    PrintBlocklist = 33,
    Testload = 34,
    GetSize = 35,
    Font = 36,
    ZfsEncryptionKey = 37,
    CryptodiskEncryptionKey = 38,
    CryptodiskDetachedHeader = 39,
    Fstest = 40,
    Mount = 41,
    FileId = 42,
    AcpiTable = 43,
    DeviceTreeImage = 44,
    Cat = 45,
    Hexcat = 46,
    Cmp = 47,
    Hashlist = 48,
    ToHash = 49,
    KeyboardLayout = 50,
    Pixmap = 51,
    GrubModuleList = 52,
    Config = 53,
    Theme = 54,
    GettextCatalog = 55,
    FsSearch = 56,
    Audio = 57,
    VbeDump = 58,

    Loadenv = 59,
    Saveenv = 60,

    VerifySignature = 61,

    Mask = 0xffff,

    SkipSignature = 0x10000,
    NoDecompress = 0x20000
}

pub struct GrubError {
    errno: ErrT,
    errmsg: String,
}

impl GrubError {
    pub fn new(num: &ErrT, msg: &str) -> Self {
	return GrubError{errno: num.clone(), errmsg: msg.to_string()};
    }

    pub fn from_env() -> Self {
	return GrubError{errno: unsafe { grub_errno.clone() }, errmsg: unsafe { CStr::from_ptr(grub_errmsg) }.to_str().unwrap().to_string()};
    }

    pub fn new_fmt(num: ErrT, args: Arguments<'_>) -> Self {
	let mut w = StrWriter {output: "".to_string()};
	let _ = fmt::write(&mut w, args);

	return GrubError{errno: num, errmsg: w.output};
    }
}

impl From<TryFromIntError> for GrubError {
    fn from(value: TryFromIntError) -> Self {
	return eformat!(ErrT::OutOfRange, "error converting value: {value:?}")
    }
}

struct GrubAllocator;

unsafe impl GlobalAlloc for GrubAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        return unsafe { grub_memalign(layout.align(), layout.size()) };
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { grub_free(ptr) };
    }
}

#[global_allocator]
static GLOBAL: GrubAllocator = GrubAllocator;

pub fn xputs(val: &str) {
    let c_to_print = CString::new(val).unwrap();

    unsafe {
	grub_xputs(c_to_print.as_ptr());
    }
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    println!("Rust runtime panicked {info:?}");
    unsafe {
	grub_abort();
    }
    loop{}
}

extern "C" fn cmd_callback (cmd: *const GrubCommand,
			    argc: c_int, argv: *const *const c_char) -> ErrT {
    let mut argv_vec: Vec<&str> = vec![];
    for i in 0..argc {
	argv_vec.push(unsafe { CStr::from_ptr(*argv.add(i as usize)) }.to_str().unwrap());
    }
    let f = unsafe { *((&(*cmd).data) as *const _ as *const fn(&[&str]) -> Result<(), GrubError>) };
    match f (&argv_vec) {
	Ok(_) => { return ErrT::None; }
	Err(e) => {
	    let c_errmsg = CString::new(e.errmsg).unwrap();

	    return unsafe {grub_error(e.errno, CStr::from_bytes_until_nul(b"%s\0").unwrap().as_ptr(), c_errmsg.as_ptr())};
	}
    }
}

pub struct Command {
    name: CString,
    summary: CString,
    description: CString,
    cmd: *mut GrubCommand,
}

impl Command {
    pub fn register(name: &str, cb: fn (argv: &[&str]) -> Result<(), GrubError>,
		    summary: &str, description: &str) -> Self {
	let mut ret = Command {
	    name: CString::new(name).unwrap(),
	    summary: CString::new(summary).unwrap(),
	    description: CString::new(description).unwrap(),
	    cmd: core::ptr::null_mut(),
	};
	unsafe {
	    ret.cmd = grub_register_command_prio (ret.name.as_ptr(),
						  cmd_callback,
						  ret.summary.as_ptr(),
						  ret.description.as_ptr(),
						  0);
	    (*ret.cmd).data = cb as *mut c_void;
	}
	return ret;
    }
}

impl Drop for Command {
    fn drop(&mut self) {
	unsafe {
	    grub_unregister_command(self.cmd);
	}
    }
}

struct PutsWriter;

impl Write for PutsWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        xputs(s);
        Ok(())
    }
}

struct StrWriter {
    output: String,
}

impl Write for StrWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.output += s;
        Ok(())
    }
}

pub fn print_fmt(args: Arguments<'_>) {
    let mut w = PutsWriter;
    let _ = fmt::write(&mut w, args);
}

pub fn real_dprintln(file: &str, line: u32, cond: &str, args: Arguments<'_>) {
    let c_cond = CString::new(cond).unwrap();

    if !unsafe { grub_debug_enabled (c_cond.as_ptr()) } {
	return;
    }
    print!("{file}:{line}:{cond}: ");
    print_fmt(args);
    unsafe { grub_refresh (); }
}

pub struct File {
    file: *mut GrubFile,
}

impl File {
    pub fn open(fname: &str, file_type: &FileType) -> Result<Self, GrubError> {
	let c_name = CString::new(fname).unwrap();
	let ret = unsafe{ grub_file_open(c_name.as_ptr(), file_type.clone()) };
	if ret == core::ptr::null_mut() {
	    return Err(GrubError::from_env());
	}
	return Ok(File { file: ret });
    }

    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize, GrubError> {
	let ret = unsafe { grub_file_read(self.file, buf.as_mut_ptr() as *mut c_void, buf.len()) };
	if ret < 0 {
	    return Err(GrubError::from_env());
	}
	return Ok(ret as usize);
    }

    pub fn seek(&mut self, off: u64) {
	unsafe { grub_file_seek(self.file, off) };
    }

    pub fn tell(&self) -> u64 {
	return unsafe { (*self.file).offset };
    }

    pub fn size(&self) -> u64 {
	return unsafe { (*self.file).size };
    }
}

impl Drop for File {
    fn drop(&mut self) {
	unsafe { grub_file_close(self.file); }
    }
}

pub trait ModuleFini {
    fn module_fini(&self);
}

pub struct ModuleCell<T> {
    inner: Cell<Option<T>>,
}

pub struct ModuleRefHolder {
    val: Cell<*const c_void>,
    destructors: RefCell<Vec<Box<dyn ModuleFini>>>,
}

impl<T> ModuleCell<T> {
    pub fn set(&'static self, dl: &ModuleRefHolder, value: T) {
	self.inner.set(Some(value));
	dl.destructors.borrow_mut().push(Box::new(self));
    }

    pub const fn new() -> Self {
	Self{inner: Cell::new(None)}
    }
}

impl<T> ModuleFini for &ModuleCell<T> {
    fn module_fini(&self) {
	self.inner.set(None);
    }
}

// SAFETY: We're single-threaded with disabled interrupts
unsafe impl<T> Sync for ModuleCell<T> {
}

impl ModuleRefHolder {
    pub fn init(&self, value: *const c_void) {
	self.val.set(value);
    }

    pub fn fini(&self) {
	for d in self.destructors.borrow().iter() {
	    d.module_fini();
	}
	self.destructors.borrow_mut().clear();
    }

    pub const fn new() -> Self {
	Self{val: Cell::new(core::ptr::null()), destructors: RefCell::new(vec![])}
    }
}

// SAFETY: We're single-threaded with disabled interrupts
unsafe impl Sync for ModuleRefHolder {
}
