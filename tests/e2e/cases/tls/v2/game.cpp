thread_local int tls_frames = 0;
static thread_local int tls_local = 100;
void update() {
    ++tls_frames;
    ++tls_local;
}
int tls_value() { return tls_frames; }
int tls_local_value() { return tls_local; }
int value() { return 2; }
