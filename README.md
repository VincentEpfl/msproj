# msproj

## setup

- gcc process1.c -o process1
- gcc process2.c stubs.c -o process2
- gcc controller.c -o controller
- gcc -shared -fPIC -o redirect.so redirect.c -ldl
- export PATH="$(pwd):$PATH"
- run with ./controller

