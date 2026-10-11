# Fork node graph regressions

`splitter_clock.c` covers graph-time sharing across splitter outputs, partial
reads, scheduled branches and route changes. `detach_iteration.cpp` pauses an
audio reader while another thread removes or reroutes its current entry. It
checks that unrelated sources still render, including when a neighboring entry
is removed before the reader resumes. No audio device or display is used.

```sh
cc -std=c89 splitter_clock.c -lm -o splitter_clock
./splitter_clock
c++ -std=c++17 -pthread detach_iteration.cpp -lm -o detach_iteration
./detach_iteration
```

NativeKit runs both as CTest audio regressions. The C++ test also supports
`-fsanitize=address,undefined -fno-omit-frame-pointer`.
