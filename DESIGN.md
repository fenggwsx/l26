# L26 编译器设计说明 (DESIGN.md)

L26 语言编译器：纯 C99 手工实现，将 L26 源码编译为类 P-Code 指令并在类
P-Code 虚拟机上解释执行。本文件是提交所需的设计说明文档。

> 实现状态见末尾「模块状态」表。本阶段交付的是**可编译、可链接、可运行**的
> 完整骨架：头文件即冻结契约，机械模块已完整实现，算法模块为可编译桩。

---

## 1. 目录结构

```
l26/
├── AGENTS.md          原始任务说明（文法 / 指令集 / 设计决策）
├── DESIGN.md          本设计文档
├── Makefile           native (l26c) + wasm 目标
├── .gitignore
├── include/           公开头文件 —— 冻结的模块间契约
│   ├── common.h       共享类型、限值、ValueType
│   ├── diag.h         诊断收集（无 I/O 副作用）
│   ├── lexer.h        TokenKind + 扫描器 API
│   ├── ast.h          AST 全部结点种类 + 构造器
│   ├── symtab.h       作用域符号表（唯一偏移分配）
│   ├── semantic.h     类型检查入口
│   ├── codegen.h      P-Code 生成入口
│   ├── vm.h           指令集 + 虚拟机（含新增集合指令）
│   └── parser.h       LALR(1) 解析入口
├── src/
│   ├── common.c       [完整] 类型名等
│   ├── diag.c         [完整] 诊断收集/格式化
│   ├── ast.c          [完整] 结点构造/释放/打印
│   ├── symtab.c       [完整] 作用域符号表
│   ├── instr.c        [完整] 指令反汇编/助记符
│   ├── lexer.c        [桩]   词法分析
│   ├── parser.c       [桩]   LALR(1) 生成器 + 驱动
│   ├── semantic.c     [桩]   语义分析
│   ├── codegen.c      [桩]   代码生成
│   ├── vm.c           [桩]   虚拟机（已接通 INT/OPR_RET）
│   └── main.c         [完整] CLI 外壳（全流程已接通）
├── tests/             L26 测试用例
│   ├── example1.l26   AGENTS.md 示例 1
│   ├── example2.l26   AGENTS.md 示例 2（作用域遮蔽）
│   └── example3.l26   集合推导式 + 集合相等 + 逻辑（附加功能）
└── web/               （后续）wasm 可视化页壳
```

核心约束：`include/` 下的所有 .c 都是纯 C、无 I/O 副作用（`ast_print` /
`program_disassemble` 例外，仅通过调用方传入的 `FILE*` 输出），可同时被原生
与 wasm 外壳链接。`main.c` 是唯一的 I/O 外壳。

---

## 2. 扩展后文法（带编号产生式）

终结符以双引号括起；`ID`、`NUM` 为词法单元。新增的**集合相等**与**集合推
导式**两条附加产生式以 `(BONUS)` 标注。文法按非终结符分层（算术 / 逻辑 /
集合），以保证 LALR(1) 无冲突。

```
(0)  program      → block
(1)  block        → "{" decls stmts "}"
(2)  decls        → decls decl
(3)  decls        → ε
(4)  decl         → type ID ";"
(5)  type         → "int"
(6)  type         → "bool"
(7)  type         → "set"

(8)  stmts        → stmts stmt
(9)  stmts        → ε
(10) stmt         → assign_stmt
(11) stmt         → if_stmt
(12) stmt         → while_stmt
(13) stmt         → io_stmt
(14) stmt         → block
(15) stmt         → set_op_stmt

(16) assign_stmt  → ID "=" expr ";"
(17) if_stmt      → "if" "(" bexpr ")" stmt              [无 else]
(18) if_stmt      → "if" "(" bexpr ")" stmt "else" stmt  [有 else]
(19) while_stmt   → "while" "(" bexpr ")" stmt
(20) io_stmt      → "write" expr ";"
(21) io_stmt      → "read" ID ";"
(22) set_op_stmt  → "add" ID aexpr ";"
(23) set_op_stmt  → "remove" ID aexpr ";"

(24) expr         → bexpr
(25) expr         → aexpr
(26) expr         → set_expr

    -- 算术 --
(27) aexpr        → aexpr "+" aterm
(28) aexpr        → aexpr "-" aterm
(29) aexpr        → aterm
(30) aterm        → aterm "*" afactor
(31) aterm        → aterm "/" afactor
(32) aterm        → afactor
(33) afactor      → ID
(34) afactor      → NUM
(35) afactor      → "(" aexpr ")"

    -- 逻辑 --
(36) bexpr        → bexpr "||" bterm
(37) bexpr        → bterm
(38) bterm        → bterm "&&" bfactor
(39) bterm        → bfactor
(40) bfactor      → "true"
(41) bfactor      → "false"
(42) bfactor      → "!" bfactor
(43) bfactor      → "(" bexpr ")"
(44) bfactor      → rel
(45) bfactor      → set_test

    -- 关系 --
(46) rel          → aexpr "<"  aexpr
(47) rel          → aexpr "<=" aexpr
(48) rel          → aexpr ">"  aexpr
(49) rel          → aexpr ">=" aexpr
(50) rel          → aexpr "==" aexpr
(51) rel          → aexpr "!=" aexpr

    -- 集合表达式 --
(52) set_expr     → "{" elemlist "}"          集合字面量 {1,2,3}
(53) set_expr     → "{" "}"                    空集字面量 {}
(54) set_expr     → ID "union" ID              并集（仅两元）
(55) set_expr     → ID "inter" ID              交集（仅两元）
(56) elemlist     → elemlist "," aexpr
(57) elemlist     → aexpr

    -- 集合测试（产出 bool）--
(58) set_test     → aexpr "in" ID              成员检查
(59) set_test     → "isempty" "(" ID ")"       空集检查

    -- 附加：集合相等判定 (BONUS) --
(60) set_test     → ID "==" ID                 集合相等  -> bool
(61) set_test     → ID "!=" ID                 集合不等  -> bool

    -- 附加：集合推导式 (BONUS) --
(62) set_expr     → "{" aexpr "|" ID "in" ID "if" bexpr "}"   带过滤
(63) set_expr     → "{" aexpr "|" ID "in" ID "}"             无过滤
```

文法说明 / 歧义消解：
- **悬挂 else**：产生式 (17)/(18)，分析器在遇到 `"else"` 时**优先移进**，
  使 else 绑定最近的 if。
- **表达式分层**：`<expr>` 在赋值 (16) 与 write (20) 处可为算术 / 逻辑 /
  集合三类之一；通过把它们拆成独立非终结符（`aexpr` / `bexpr` /
  `set_expr`），并让语义分析依据左值或上下文类型选择，避免规约冲突。
- **集合相等的两套写法**：标量 `==`/`!=` 走关系产生式 (50)/(51)（操作数为
  `aexpr`）；集合 `==`/`!=` 走 (60)/(61)（操作数为集合 `ID`），由语义阶段
  按操作数类型区分，二者文法上以操作数非终结符不同而分离。
- `union`/`inter` 仅两元（设计决策 #6），不支持链式。

LALR(1) 表在**运行时**由上述带编号产生式构建：先求规范 LR(1) 项目集族
（闭包 + GOTO），再合并同心状态得到 LALR(1) 的 ACTION/GOTO 表，最后以显式
「状态栈 + 符号(Node\*)栈」驱动分析，每次规约执行一个构建 AST 的语义动作。
`parser_build_tables_report()` 可在自测中断言文法保持无冲突。

---

## 3. 单活动记录作用域模型

（设计决策 #3，AGENTS.md 示例 2 的语义来源）

- 整个程序视为**一个过程 / 一条活动记录**。
- 符号表为**每一个声明**分配**唯一且单调递增**的存储偏移——即使是遮蔽
  内层声明，也获得独立偏移，不与被遮蔽者复用。
- 名字解析在**编译期**选取「当前仍打开的、最内层」声明（`symtab_resolve`
  自新到旧扫描首个 `active` 同名符号）。
- 块退出时，其内层符号仅被标记 `active=0`（**偏移不回收**），此后查找不再
  命中它们，外层同名符号自动「恢复可见」。
- 程序入口处仅发射**一条** `INT 0 A`，`A = symtab_frame_size()` 为所有声明
  尺寸之和。VM 无需任何运行期作用域压栈/弹栈。

示例 2 中内层 `set x` 与外层 `int x` 各占独立偏移：内层块内对 `x` 的引用解
析到集合偏移，内层块结束后对 `x` 的引用恢复解析到外层 int 偏移——全部在编
译期由符号表完成。

---

## 4. 集合的内联 201 单元内存布局

（设计决策 #4/#5/#7）

`int` / `bool` 各占 **1** 个栈单元。`set` 在活动记录中**内联占用 201 个单
元**：

```
偏移 base+0          : 元素个数 count (0 .. 200)
偏移 base+1 .. base+200 : 至多 200 个元素值，按升序存放、去重
```

即 `L26_SET_CELLS = 1 + L26_MAX_SET = 201`。元素恒定**升序去重**，使得相
等判定 / 并集 / 交集 / 成员检查 / 添加 / 删除均可线性且确定地完成。符号表
记录每个变量的 `base` 偏移；集合指令以该 `base` 偏移作为操作数 `a`。

---

## 5. 完整指令系统

指令编码为 `{ VmOp op; int l; int a; }`（`op` 为枚举而非 3 字符串，使
codegen 与 VM 共用同一符号）。`l` 为层差（单活动记录下恒为 0，但保留以遵循
基础 CAL/LOD/STO 格式）。`a` 为地址 / 立即数 / 偏移 / OPR 子功能号。

### 5.1 基础指令（与 AGENTS.md 完全一致）

| 助记 | 格式 | 功能 / 栈效应 |
| --- | --- | --- |
| INT | `INT 0 A` | 栈顶开辟 A 个单元（活动记录） |
| OPR | `OPR 0 0` | 结束过程 / 停机（OPR_RET） |
| CAL | `CAL L A` | 调用地址 A 的过程（保留，单活动记录下未用） |
| LIT | `LIT 0 A` | 立即数 A 入栈，t+1 |
| LOD | `LOD L A` | 偏移 A 单元值入栈，t+1 |
| STO | `STO L A` | 栈顶存入偏移 A 单元，t−1 |
| OPR | `OPR 0 1` | 栈顶取负（NEG） |
| OPR | `OPR 0 6` | 栈顶奇为 1 偶为 0（ODD） |
| OPR | `OPR 0 2..5` | 次顶 ⊕ 顶：加/减/乘/除，结果入次顶，t−1 |
| OPR | `OPR 0 8..13` | 次顶 ⊕ 顶比较：==/!=/</>=/>/<=，结果 0/1 入次顶，t−1 |
| JMP | `JMP 0 A` | 无条件转移至 A |
| JPC | `JPC 0 A` | 栈顶为 0 则转移至 A，t−1 |
| OPR | `OPR 0 14` | 栈顶输出（int），t−1 |
| OPR | `OPR 0 15` | 输出换行 |
| OPR | `OPR 0 16` | 读入一行整数入栈，t+1 |

OPR 子功能号在 `vm.h` 中以 `OprFunc` 枚举命名（`OPR_ADD=2` … `OPR_READ=16`），
取值与上表一致。

### 5.2 新增集合指令（设计决策 #7：新增操作码，绝不复用整数操作码）

所有集合指令以集合区**基偏移**寻址（在操作数 `a` 中或经栈传递）。栈是 int
单元；集合指令直接读写活动记录中的 201 单元区。

| 助记 | 格式 | 功能 / 精确栈效应 |
| --- | --- | --- |
| SCLR   | `SCLR 0 A`   | 将 A 处集合区置空（count=0）。无栈效应。 |
| SADD   | `SADD 0 A`   | 弹出 v；将 v 插入 A 处集合（去重、保持升序、上限 200，越界报错）。t−1。 |
| SREM   | `SREM 0 A`   | 弹出 v；从 A 处集合删除 v（不存在则不变）。t−1。 |
| SIN    | `SIN 0 A`    | 弹出 v；若 v ∈ 集合 A 则压 1 否则压 0。净栈效应 0（弹 v 压 bool）。 |
| SEMPTY | `SEMPTY 0 A` | 若集合 A 为空压 1 否则压 0。t+1。 |
| SUNION | `SUNION 0 A` | 二元并集。先后压入左、右两个**集合基偏移**(整数)，弹出二者，结果写入 A 处集合。净 t−2。 |
| SINTER | `SINTER 0 A` | 二元交集，协议同 SUNION，结果写入 A。净 t−2。 |
| SCOPY  | `SCOPY 0 A`  | 弹出一个源集合基偏移；将其整块 201 单元复制到 A 处集合。t−1。 |
| SEQ    | `SEQ 0 A`    | 弹出左、右两个集合基偏移；相等压 1 否则压 0。净 t−1（弹 2 压 1）。A 未用。 |
| SWRITE | `SWRITE 0 A` | 将 A 处集合按 `{e1,e2,...}` 输出。无栈效应。 |
| SREAD  | `SREAD 0 A`  | 读入一行（空格/逗号分隔整数）置入 A 处集合（先清空）。无栈效应。 |

> 设计取舍：`SUNION`/`SINTER`/`SCOPY`/`SEQ` 用栈传递**集合基偏移**而非把
> 201 个值搬上操作数栈，既复用了 int 栈又避免巨量压栈；结果集合直接落到目
> 标区，契合「集合内联于活动记录」的布局。

### 5.3 附加功能到指令的映射

- **集合相等判定**（产生式 60/61）：`LIT base_l; LIT base_r; SEQ`；`!=` 再
  对结果做布尔取反。
- **集合推导式**（产生式 62/63）`{ gen | x in src if filt }`：降级为对源集
  合元素的循环——为 `x` 分配一个临时 int 单元（推导式专属、仅内部可见），
  先 `SCLR dest`，遍历 `src` 每个元素：将元素载入 `x`，求值 `filt`（无则恒
  真）→ 若真则求值 `gen` 并 `SADD dest`。循环用 LOD/比较/JPC/JMP 配合
  `src` 的 count 单元实现（详见 codegen.c TODO）。

---

## 6. 编译流水线

```
源文本 ──lexer──▶ TokenStream ──parser(LALR)──▶ AST
   ──semantic(符号表/类型)──▶ 带类型注解的 AST + 帧布局
   ──codegen──▶ Program(P-Code) ──vm──▶ 执行 / 输出
```

诊断贯穿全程收集于 `DiagList`（无 I/O），由外壳统一渲染。`read`/`write` 不
限类型：写集合输出 `{...}`；写 bool 输出其整数值（0/1，本实现选择数值，文
档化于此）；读取按整数行解析（集合变量用 `SREAD` 读多个整数）。

---

## 7. 构建与运行

前置：任意 C99 编译器（`cc` / `clang` / `gcc`）。无外部依赖。

```sh
make                 # 构建原生 CLI，产出 ./l26c
make clean           # 清理
make wasm            # Emscripten 构建（emcc 缺失时友好跳过，不报错）
```

运行：

```sh
./l26c tests/example1.l26          # 编译并运行（默认）
./l26c -a tests/example2.l26       # 打印 AST 后停止
./l26c -S tests/example3.l26       # 打印反汇编 P-Code 后停止
./l26c -                           # 从标准输入读取源码
echo 4 | ./l26c tests/example3.l26 # 通过管道喂入 read 的输入
```

返回码：0 成功；1 编译/运行错误；2 用法错误。

---

## 8. 模块状态

| 文件 | 角色 | 状态 |
| --- | --- | --- |
| include/*.h | 模块间契约（全部头文件） | header（冻结） |
| src/common.c | ValueType 名称等 | 完整 |
| src/diag.c | 诊断收集/格式化 | 完整 |
| src/ast.c | AST 构造/释放/打印 | 完整 |
| src/symtab.c | 作用域符号表/偏移分配 | 完整 |
| src/instr.c | 指令助记符/反汇编 | 完整 |
| src/main.c | CLI 外壳（全流程已接通） | 完整 |
| src/lexer.c | 词法分析 | 桩（TODO） |
| src/parser.c | LALR(1) 生成器 + 驱动 | 桩（TODO） |
| src/semantic.c | 语义分析/类型检查 | 桩（TODO） |
| src/codegen.c | P-Code 代码生成 | 桩（TODO） |
| src/vm.c | 虚拟机（已接通 INT/OPR_RET） | 桩（TODO） |

实现者各自只需填写**一个** .c 的函数体（可新增 static 辅助函数），不得改动
头文件或彼此的文件。头文件已尽量充分；若发现缺漏请先在评审中扩充契约。
