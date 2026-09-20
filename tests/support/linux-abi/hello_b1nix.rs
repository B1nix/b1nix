use std::collections::HashMap;
fn main() {
    let mut v: Vec<i32> = Vec::new();
    for i in 0..5 { v.push(i * i); }
    let s = String::from("rust on b1nix");
    let mut m = HashMap::new();
    m.insert("sum", v.iter().sum::<i32>());
    println!("{} squares={:?} sum={}", s, v, m["sum"]);
    // thread + mutex (exercises futex-based sync)
    let h = std::thread::spawn(|| 42u64);
    let r = h.join().unwrap();
    println!("thread returned {}", r);
}
