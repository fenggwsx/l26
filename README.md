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

## 测试

```sh
bash tests/run_tests.sh             # 构建并运行全部测试（28/28 通过）
```
