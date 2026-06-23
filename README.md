# L26 编译器

L26 语言编译器：纯 C99 手工实现，将 L26 源码编译为类 P-Code 指令，并在自研类
P-Code 虚拟机上解释执行。

完整设计说明（扩展后文法、模块结构、指令系统、内存 / 作用域模型、设计约束、
测试结果）见 **[DESIGN.md](DESIGN.md)**。

## 快速开始

```sh
make                                # 构建原生 CLI，产出 ./l26c
./l26c tests/example1.l26           # 编译并运行（默认）
./l26c -a tests/example2.l26        # 打印 AST 后停止
./l26c -S tests/example3.l26        # 打印反汇编 P-Code 后停止
echo 5 | ./l26c tests/example3.l26  # 通过管道喂入 read 的输入
./l26c -                            # 从标准输入读取源码

make clean                          # 清理
make wasm                           # Emscripten 构建（emcc 缺失时跳过）
```

命令行选项：`-a/--ast`、`-S/--asm`、`-r/--run`（默认）、`-h/--help`。
返回码：0 成功；1 编译 / 运行错误；2 用法错误。

## Web 可视化演示

`l26web` 是一个独立的 Go 程序，把 `web/` 下的静态资源（含 emscripten 产出的
`l26.js` / `l26.wasm`）内嵌进单个二进制并通过 HTTP 伺服，与编译器本体 `l26c`
职责分离。net/http 为每个连接起一个 goroutine，浏览器并发拉取资源不会阻塞。

```sh
make wasm                           # 先生成 web/l26.js + web/l26.wasm（需 emcc）
make webserver                      # 构建 ./l26web（内嵌 web/ 静态资源）
./l26web                            # 监听 127.0.0.1:8080，打印访问 URL
./l26web -port 9000                 # 指定端口
./l26web -addr 0.0.0.0              # 对外暴露（默认仅本机）
```

## 测试

```sh
bash tests/run_tests.sh             # 构建并运行全部测试（28/28 通过）
```
