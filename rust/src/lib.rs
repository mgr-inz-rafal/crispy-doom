#[no_mangle]
pub extern "C" fn rs_init() -> i32 {
    println!("Greetings from Rust!");
    0
}
