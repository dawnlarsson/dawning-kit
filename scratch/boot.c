#include "../src/library.c"

fn log(address_any data, positive length);
fn log_flush();

b32 main()
{
        log("ok\n", 3);
        log_flush();
        return 0;
}
