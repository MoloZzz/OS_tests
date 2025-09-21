## Як запускати

В одному терміналі:
```
gcc server.c -o server
./server --unix --blocking
```

В іншому терміналі:
```
gcc client.c -o client
./client --unix --small
```

Приклади конфігурацій:
```
./server --inet --blocking + ./client --inet --large
```
```
./server --unix --nonblocking + ./client --unix --small
```
## Це базова демка. Вона вже:
- працює для UNIX/INET;
- дозволяє перемикати blocking/nonblocking;
- міряє час створення сокета, connect/accept, закриття;
- тестує 2 типи навантаження (багато дрібних / мало великих пакетів).