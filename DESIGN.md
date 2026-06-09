# L26 编译器设计说明（DESIGN.md）

L26 语言编译器：纯 C99 手工实现，将 L26 源码编译为类 P-Code 指令，并在自研
类 P-Code 虚拟机上解释执行。本文件是提交所需的「编译器设计说明文档」，内容
包括：扩展后文法、代码结构与模块说明、完整指令系统、内存与作用域模型、构建
与运行方式、测试结果。

> 实现状态：**全部模块均已完整实现**，词法 / 语法（LALR(1)）/ 语义 / 代码生成 /
> 虚拟机端到端贯通；测试套件 **28/28 全部通过**（见第 11 节）。

---

## 1. 概述与设计决策

L26 是一门带 `int` / `bool` / `set` 三种类型的小型块结构语言，支持赋值、
`if` / `while`、`read` / `write`、集合操作（`add` / `remove` / `union` /
`inter` / `in` / `isempty`）以及两项附加特性（**集合相等判定**与**集合推导
式**）。编译器把源码翻译成类 P-Code 指令，再由内置虚拟机解释执行。

下表对照 AGENTS.md「设计决策」表逐条说明本实现的落地情况：

| # | 决策项 | 本实现结论 |
| --- | --- | --- |
| 1 | 编译器实现方式 | **纯 C99 手工实现**，无第三方依赖（`make` 即可构建）。 |
| 2 | 语法分析算法 | **手写表驱动 LALR(1)**：运行时构造规范 LR(1) 项集族，合并同核状态得 LALR(1) ACTION/GOTO 表后驱动分析（见第 4 节）。 |
| 3 | 交付形态 | 主交付为**原生命令行版** `l26c`；`make wasm` 提供 Emscripten 目标（缺 `emcc` 时友好跳过），Web 可视化页壳为后续加分项。 |
| 4 | 编译目标 | **不可变**：L26 → 类 P-Code → 类 P-Code 虚拟机解释执行。 |
| 5 | LSP | 前端分析核做成无 I/O 副作用的诊断库（`DiagList`），外壳层可复用；具体 LSP 壳推迟选型。 |
| 6 | union / inter | **仅两元**：一条集合表达式至多一个二元集合运算符，不支持链式。 |
| 7 | read / write 类型 | **均不限类型**：`int` / `bool` / `set` 都可读写。 |
| 8 | 附加功能 | 已实现**集合相等判定**与**集合推导式**两项；Web 可视化演示为后续项。 |

架构遵循「一个分析核 + 多个外壳」：前端分析核、代码生成器、虚拟机均为纯 C、
无 I/O 副作用（仅 `ast_print` / `program_disassemble` 通过调用方传入的
`FILE*` 输出），可被原生 CLI 与未来 wasm 外壳共同链接，核心逻辑只有一份。

---

## 2. 目录结构

```
l26/
├── AGENTS.md          原始任务说明（文法 / 指令集 / 设计决策）
├── DESIGN.md          本设计文档
├── README.md          快速上手（指向本文档与构建命令）
├── Makefile           native (l26c) + wasm 目标
├── include/           公开头文件 —— 模块间契约
│   ├── common.h       共享类型、限值、ValueType、SrcPos
│   ├── diag.h         诊断收集（无 I/O 副作用）
│   ├── lexer.h        TokenKind + 扫描器 API
│   ├── ast.h          AST 全部结点种类 + 构造器
│   ├── symtab.h       作用域符号表（唯一偏移分配）
│   ├── semantic.h     类型检查入口
│   ├── codegen.h      P-Code 生成入口
│   ├── vm.h           指令集 + 虚拟机（含新增集合指令）
│   └── parser.h       LALR(1) 解析入口
├── src/               实现（全部已完成）
│   ├── common.c       ValueType 名称 / 尺寸
│   ├── diag.c         诊断收集 / 格式化
│   ├── ast.c          AST 构造 / 释放 / 打印
│   ├── symtab.c       作用域符号表 / 偏移分配
│   ├── instr.c        指令助记符 / 反汇编
│   ├── lexer.c        词法分析
│   ├── parser.c       LALR(1) 表生成 + 表驱动分析 + 建 AST
│   ├── semantic.c     语义分析 / 类型检查 / 符号表
│   ├── codegen.c      P-Code 代码生成
│   ├── vm.c           类 P-Code 虚拟机
│   └── main.c         CLI 外壳（全流程贯通）
├── tests/             测试用例 + run_tests.sh（见第 11 节）
└── web/               （后续）wasm 可视化页壳
```

逐文件职责详见第 10 节模块表。

---

## 3. 编译流水线

```
源文本 ──lexer──▶ TokenStream ──parser(LALR(1))──▶ AST
   ──semantic(符号表/类型)──▶ 带类型注解的 AST + 帧布局
   ──codegen──▶ Program(P-Code) ──vm──▶ 执行 / 输出
```

- **词法分析**（`lexer.c`）：把源码扫描成 `TokenStream`，识别关键字、标识符、
  整数、运算符与定界符，记录每个 token 的 `SrcPos`（行 / 列 / 偏移）。注释
  `// ...` 在词法阶段丢弃。
- **语法分析**（`parser.c`）：表驱动 LALR(1) 分析，规约时执行语义动作构建
  AST（见第 4 节）。
- **语义分析**（`semantic.c`）：遍历 AST，借助作用域符号表（`symtab.c`）做声
  明 / 类型检查、名字解析与帧布局，给表达式结点标注静态类型。
- **代码生成**（`codegen.c`）：消费带类型的 AST，产出 `Program`（P-Code 指令
  序列），含集合指令降级与短路求值。
- **虚拟机**（`vm.c`）：解释执行 P-Code，整数与集合统一在单活动记录中运行。

诊断贯穿全程，统一收集于 `DiagList`（无 I/O 副作用），由 `main.c` 外壳渲染到
`stderr`；任何阶段出错即停止后续阶段，进程返回非零退出码。

---

## 4. 语法分析：表驱动 LALR(1)

`parser.c` 在**运行时**按第 5 节带编号文法构造分析表，流程如下：

1. 以增广文法 `S' → program` 为起点，求**规范 LR(1) 项目集族**（`closure` +
   `goto_set`）。LR(1) 项 = (产生式, 圆点位置, 向前看终结符)；同一 (产生式,
   圆点) 的多个向前看符被合并存放。
2. 增量构造状态集合：当 `GOTO` 产生的新集合其 **LR(0) 核**与已有状态相同时，
   把向前看符**合并**进既有状态（`merge_la`），即由规范 LR(1) 合并同核得到
   **LALR(1)**；若合并改变了向前看集则重新处理该状态。
3. 填充 **ACTION**（移进 / 规约 / 接受）与 **GOTO** 表；冲突会被检出并报告。
4. 以显式「状态栈 + 符号（`Node*`）栈」驱动分析，每次规约执行一个构建 AST 的
   语义动作。

文法设计 / 歧义消解：

- **悬挂 else**：产生式 (17)/(18)，分析器遇到 `else` 时**优先移进**，使 else
  绑定到最近的 `if`。
- **表达式分层**：`expr` 在赋值 (16) 与 `write` (20) 处可为算术 / 逻辑 / 集合
  三类之一；通过把它们拆成独立非终结符（`aexpr` / `bexpr` / `set_expr`）并按
  操作数非终结符区分，避免规约冲突，保证 LALR(1) 无冲突。
- **集合相等折叠进关系产生式**：`==` / `!=` 在文法上**只有一条路径**——关系产
  生式 (50)/(51)（操作数为 `aexpr`），并不为集合相等单列产生式。原因是无法把
  `set_test → ID == ID` 与标量比较在文法上分离：当分析栈顶为裸 `ID`、向前看符为
  `==` 时，它会与 `afactor → ID` 的规约动作构成**无法消解的移进 / 规约冲突**（两
  条路径的左前缀都是裸 `ID`，LALR(1) 在该点无从区分）。故集合相等统一走 (50)/(51)：
  规约时若 `==` / `!=` 两侧均为裸变量则乐观建成 `N_SETEQ` 结点，否则建 `N_REL`；
  语义阶段（`check_seteq`）再按符号表中操作数的实际类型定夺——两侧皆 `set` 即真正
  的集合相等（产出 bool，附加功能），两侧皆标量则就地回写为 `N_REL` 交由整数比较码
  降级，一 set 一标量等混合则报类型错。
- `union` / `inter` 仅两元（决策 #6），不支持链式。

---

## 5. 扩展后文法（带编号产生式）

终结符以双引号括起；`ID`、`NUM` 为词法单元。新增的**集合推导式**(60/61)以
`(BONUS)` 标注；另一项附加功能**集合相等判定**不占独立产生式，而是折叠进关系产
生式 (50)/(51)，由语义阶段按操作数类型识别（理由见第 4 节）。文法按非终结符分层
（算术 / 逻辑 / 集合）以保证 LALR(1) 无冲突。

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
(17) if_stmt      → "if" "(" bexpr ")" stmt               [无 else]
(18) if_stmt      → "if" "(" bexpr ")" stmt "else" stmt   [有 else]
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
    集合 == / != 不设独立产生式，统一折叠进关系产生式 (50)/(51)，
    由语义阶段按操作数类型识别为集合相等或标量比较（见第 4 节）。

    -- 附加：集合推导式 (BONUS) --
(60) set_expr     → "{" aexpr "|" ID "in" ID "if" bexpr "}"   带过滤
(61) set_expr     → "{" aexpr "|" ID "in" ID "}"              无过滤
```

集合测试 (58)/(59) 归约为 `bfactor`（产生式 45 路径），集合相等折叠进 `rel`
后归约为 `bfactor`（产生式 44 路径），因此 `in` / `isempty` / 集合相等都可作布尔
因子参与 `&& || !` 组合。

---

## 6. 单活动记录作用域模型

（决策 #3；AGENTS.md 示例 2 的语义来源）

- 整个程序视为**一个过程 / 一条活动记录**。
- 符号表（`symtab.c`）为**每一个声明**（含被遮蔽的）分配**唯一且单调递增**的
  存储偏移——遮蔽内层声明也获得独立偏移，不与被遮蔽者复用。
- 名字解析在**编译期**选取「当前仍打开的、最内层」声明（`symtab_resolve` 自
  新到旧扫描首个 `active` 同名符号）。
- 块退出时其内层符号仅被标记 `active=0`（**偏移不回收**），此后查找不再命中
  它们，外层同名符号自动「恢复可见」。**VM 无任何运行期作用域压栈 / 退栈。**
- 程序入口处仅发射**一条** `INT 0 A`，`A = symtab_frame_size()` 为所有声明
  尺寸之和（`int` / `bool` 各 1 单元，`set` 201 单元）。

以 AGENTS.md 示例 2 为例：内层 `set x` 与外层 `int x` 各占独立偏移；内层块内
对 `x` 的引用解析到集合偏移，内层块结束后对 `x` 的引用恢复解析到外层 int 偏
移——全部在编译期由符号表完成。运行 `tests/example2.l26` 输出
`{5,6,7}` / `{1,2}` / `10`，与示例预期一致。

---

## 7. 集合的内联 201 单元内存布局

（决策 #4 / #7）

`int` / `bool` 各占 **1** 个栈单元；`set` 在活动记录中**内联占用 201 个单
元**：

```
偏移 base+0              : 元素个数 count (0 .. 200)
偏移 base+1 .. base+200  : 至多 200 个元素值，按升序存放、去重
```

即 `L26_SET_CELLS = 1 + L26_MAX_SET = 201`。元素恒定**升序去重**，使相等判定 /
并集 / 交集 / 成员检查 / 添加 / 删除均可线性且确定地完成（`vm.c` 中的
`set_find` 用二分查找，`set_add` / `set_remove` 维护有序去重不变式）。符号表
记录每个集合变量的 `base` 偏移，集合指令以该 `base` 作操作数 `a`，或经操作数
栈传递 `base`（用于并 / 交 / 拷贝 / 相等）。

---

## 8. 完整指令系统

指令编码为 `{ VmOp op; int l; int a; }`（`op` 为枚举而非 3 字符串，使 codegen
与 VM 共用同一符号）。`l` 为层差（单活动记录下恒为 0，保留以遵循基础
CAL/LOD/STO 格式）。`a` 为地址 / 立即数 / 偏移 / OPR 子功能号。操作数栈与单活
动记录共用同一 int 数组（基址 `b = 0`）。

### 8.1 基础指令（与 AGENTS.md 完全一致）

| 助记 | 格式 | 功能 / 栈效应 |
| --- | --- | --- |
| INT | `INT 0 A` | 栈顶开辟 A 个单元（活动记录），置 0 |
| OPR | `OPR 0 0` | 结束过程 / 停机（OPR_RET） |
| CAL | `CAL L A` | 调用地址 A 的过程（保留，单活动记录下不发射，VM 视作空操作） |
| LIT | `LIT 0 A` | 立即数 A 入栈，t+1 |
| LOD | `LOD L A` | 偏移 A 单元值入栈，t+1 |
| STO | `STO L A` | 栈顶存入偏移 A 单元，t−1 |
| OPR | `OPR 0 1` | 栈顶取负（NEG） |
| OPR | `OPR 0 6` | 栈顶奇为 1 偶为 0（ODD） |
| OPR | `OPR 0 2..5` | 次顶 ⊕ 顶：加 / 减 / 乘 / 除，结果入次顶，t−1（除法向零截断，除零报运行时错） |
| OPR | `OPR 0 8..13` | 次顶 ⊕ 顶比较：== / != / < / >= / > / <=，结果 0/1 入次顶，t−1 |
| JMP | `JMP 0 A` | 无条件转移至 A |
| JPC | `JPC 0 A` | 栈顶为 0 则转移至 A，t−1 |
| OPR | `OPR 0 14` | 栈顶输出（int），t−1 |
| OPR | `OPR 0 15` | 输出换行 |
| OPR | `OPR 0 16` | 读入一个整数入栈，t+1 |

OPR 子功能号在 `vm.h` 中以 `OprFunc` 枚举命名（`OPR_RET=0` … `OPR_READ=16`），
取值与上表一致；`instr.c` 反汇编时附上助记注释（如 `OPR 0 14 ; write`）。

### 8.2 新增集合指令（决策 #7：新增 12 个操作码，绝不复用整数操作码）

集合指令直接读写活动记录中的 201 单元区；以集合区**基偏移**寻址（在操作数
`a` 中，或经操作数栈传递）。下表栈效应与 `vm.c` 实现逐条核对：

| 助记 | 格式 | 功能 / 精确栈效应 |
| --- | --- | --- |
| SCLR   | `SCLR 0 A`   | 将 A 处集合置空（count=0）。无栈效应。 |
| SADD   | `SADD 0 A`   | 弹出 v；将 v 插入 A 处集合（去重、保持升序、上限 200，越界报运行时错）。t−1。 |
| SREM   | `SREM 0 A`   | 弹出 v；从 A 处集合删除 v（不存在则不变）。t−1。 |
| SIN    | `SIN 0 A`    | 弹出 v；若 v ∈ 集合 A 则压 1 否则压 0。净栈效应 0（弹 v 压 bool）。 |
| SEMPTY | `SEMPTY 0 A` | 若集合 A 为空压 1 否则压 0。t+1。 |
| SUNION | `SUNION 0 A` | 二元并集。依次压入左、右两个**集合基偏移**(整数)，弹出二者，结果写入 A 处集合。净 t−2（dest 可与操作数别名，先算入临时缓冲再落地）。 |
| SINTER | `SINTER 0 A` | 二元交集，协议同 SUNION，结果写入 A。净 t−2。 |
| SCOPY  | `SCOPY 0 A`  | 弹出一个源集合基偏移；将其整块 201 单元复制到 A 处集合。t−1。 |
| SEQ    | `SEQ 0 A`    | 弹出左、右两个集合基偏移；相等压 1 否则压 0。净 t−1（弹 2 压 1）。操作数 A 未用。 |
| SWRITE | `SWRITE 0 A` | 将 A 处集合按 `{e1,e2,...}`（升序去重、无空格，空集为 `{}`）输出。无栈效应。 |
| SREAD  | `SREAD 0 A`  | 读入一行（空格 / 逗号分隔整数）置入 A 处集合（先清空、去重升序）。无栈效应。 |
| LODX   | `LODX 0 0`   | 间接取数：弹出一个**帧偏移**，压入该偏移处单元的值。净栈效应 0（弹偏移压值）。供推导式按运行时下标遍历集合区。 |

> 设计取舍：`SUNION` / `SINTER` / `SCOPY` / `SEQ` 用操作数栈传递**集合基偏移**
> 而非把 201 个值搬上栈，既复用 int 栈又避免巨量压栈；结果集合直接落到目标
> 区，契合「集合内联于活动记录」的布局。

### 8.3 附加功能到指令的映射

- **集合相等判定**（折叠进关系产生式 50/51，语义阶段识别为 `N_SETEQ`）：
  `LIT base_l; LIT base_r; SEQ`；`!=` 再对 bool 结果做逻辑取反。
- **集合推导式**（产生式 60/61）`{ gen | x in src if filt }`：降级为对源集合
  `src` 元素的**运行时循环**——为推导变量 `x` 和隐藏循环下标 `i` 各分配一个
  临时 int 单元（**仅推导式内部可见**）：

  ```
        SCLR tmp                ; tmp := {}
        LIT 1 ; STO i           ; i := 1
  loop: LOD i ; LOD src
        OPR le ; JPC end        ; while (i <= count)
        LIT src ; LOD i
        OPR add ; LODX          ; 压入 cell[src+i]（间接取数）
        STO x                   ; x := 当前元素
        [filt] ; JPC next       ; 过滤（无过滤则省略）
        [gen]  ; SADD tmp       ; tmp += gen(x)
  next: LOD i ; LIT 1 ; OPR add ; STO i
        JMP loop
  end:  LIT tmp ; SCOPY dest
  ```

  整个推导式仅生成 ~20 条固定指令（外加 gen/filt 自身的代码），与集合容量
  L26_MAX_SET 无关；`LODX` 提供 LOD 所缺的运行时下标寻址能力。结果先写入临
  时集 `tmp` 再 `SCOPY` 到 `dest`，避免在遍历 `src` 时 `dest` 与 `src` 别名
  导致读写冲突。

### 8.4 反汇编样例（`./l26c -S tests/example1.l26` 前段）

```
   0  INT    0 202
   1  SCLR   0 1
   2  LIT    0 1
   3  SADD   0 1
   4  LIT    0 2
   5  SADD   0 1
   6  LIT    0 3
   7  SADD   0 1
   8  OPR    0 16   ; read
   9  STO    0 0
  10  LOD    0 0
  11  SIN    0 1
  12  JPC    0 18
  ...
```

`INT 0 202` = 1 个 int（`val`，偏移 0）+ 201 个 set 单元（`s`，基偏移 1）。

---

## 9. I/O 语义与读写约定

`read` / `write` 均不限类型（决策 #7）：

- `write` 整数：输出十进制值；`write` 布尔：输出其整数值 **1/0**；`write` 集合：
  输出 `{e1,e2,...}`（升序去重无空格），空集为 `{}`。每条 `write` 之后发射
  `OPR 0 15` 追加一个换行，故每次写入独占一行。
- `read` 标量：从输入按空白 / 逗号分隔取下一个整数（`bool` 读入按非零即真存
  储）。`read` 集合：消费输入的整行，按空白 / 逗号切分多个整数置入集合（先清
  空，自动去重升序）。`read` **不回显**输入。
- 整数除法**向零截断**（如 `-7 / 2 = -3`）；除零、栈溢出、集合越界等在运行期
  报告诊断并停止执行。
- 诊断统一输出到 **stderr**，并使进程返回**非零退出码**；正常输出走 stdout。

---

## 10. 模块结构表（逐文件职责）

| 文件 | 角色 | 说明 |
| --- | --- | --- |
| include/*.h | 模块间契约 | 全部公开头文件，定义类型与 API（冻结） |
| src/common.c | 类型工具 | `value_type_name` / 类型尺寸 |
| src/diag.c | 诊断库 | `DiagList` 收集 / 格式化，无 I/O 副作用 |
| src/ast.c | AST | 结点构造 / 释放 / 打印（`-a` 用） |
| src/symtab.c | 符号表 | 作用域进出、唯一偏移分配、编译期名字解析、帧尺寸 |
| src/instr.c | 反汇编 | 助记符 / OPR 名 / 单条与整程序反汇编（`-S` 用） |
| src/lexer.c | 词法分析 | 源码 → `TokenStream`，记录 `SrcPos`，丢弃注释 |
| src/parser.c | 语法分析 | 运行时构 LALR(1) 表 + 表驱动分析 + 语义动作建 AST |
| src/semantic.c | 语义分析 | 声明 / 类型检查、名字解析、帧布局、类型标注 |
| src/codegen.c | 代码生成 | AST → P-Code，含集合降级与短路求值 |
| src/vm.c | 虚拟机 | 解释执行全部基础指令 + 11 个集合指令与 LODX 间接取数；含单步 `vm_step` |
| src/main.c | CLI 外壳 | 解析参数、串接流水线、渲染诊断、设置退出码 |

短路求值：`&&` / `||` 由 `codegen.c` 用 JPC/JMP 实现，右操作数在结果已定时
**不被求值**（避免逻辑上被跳过的分支触发副作用 / 运行时错误）。

---

## 11. 设计约束 / 已知限制

以下两条是**严格遵循 AGENTS.md 给定 EBNF 文法**的有意结果（已确认保持文法不
变），并非缺陷，特此如实记录：

1. **裸 `bool` 变量不能直接作布尔因子。** 文法 `<bfactor>`（产生式 40–45）**没
   有 `bfactor → ID` 产生式**，因此形如 `if (p)`（`p` 为 bool 变量）**不合
   法**，会报语法错误。布尔上下文须用关系式（`a>0`）、集合测试（`x in s` /
   `isempty(s)` / 集合相等）、`true` / `false` 经 `&&` / `||` / `!` 组合表达。
   `bool` 变量仍可作**赋值目标**（`p = a > 0;`）与 **`write` 操作数**
   （`write p;` 输出 1/0）。示例见 `tests/programs/bool_logic.l26`。

2. **无一元负号。** 文法 `<afactor>`（产生式 33–35）**不含一元 `-`**，负数须写
   成 `0 - 5` 形式。示例见 `tests/edge/negatives.l26`。

其他限制：集合最多 200 个元素（`L26_MAX_SET`）；`union` / `inter` 仅两元（决
策 #6）；集合 `==` / `!=` 与标量 `==` / `!=` 共用关系产生式，语义阶段按操作数类
型区分（不在文法层分离，见第 4 节）。

---

## 12. 构建与运行

前置：任意 C99 编译器（`cc` / `clang` / `gcc`），无外部依赖。

```sh
make                 # 构建原生 CLI，产出 ./l26c
make clean           # 清理
make wasm            # Emscripten 构建（emcc 缺失时友好跳过，不报错）
```

运行：

```sh
./l26c tests/example1.l26           # 编译并运行（默认）
./l26c -a tests/example2.l26        # 打印 AST 后停止
./l26c -S tests/example3.l26        # 打印反汇编 P-Code 后停止
./l26c -                            # 从标准输入读取源码
echo 5 | ./l26c tests/example3.l26  # 通过管道喂入 read 的输入
```

命令行选项：`-a/--ast`（打印 AST）、`-S/--asm`（打印 P-Code）、`-r/--run`（编
译并运行，默认）、`-h/--help`。返回码：0 成功；1 编译 / 运行错误；2 用法错误。

---

## 13. 测试结果

测试由 `tests/run_tests.sh` 驱动：先 `make`，再分三类执行。每个正向用例把程序
stdout 与同名 `.expected` 比对（存在同名 `.in` 时作为 stdin 喂入）；每个错误用
例必须被**拒绝**（非零退出 + stderr 含 `error` 诊断）。

测试套件构成（共 **28** 项）：

| 类别 | 目录 | 数量 | 内容 |
| --- | --- | --- | --- |
| 示例 | `tests/example*.l26` | 3 | AGENTS.md 示例 1、示例 2（作用域遮蔽）、示例 3（集合推导式 + 集合相等 + 逻辑） |
| 程序 | `tests/programs/*.l26` | 6 | factorial、gcd、prime_sieve（埃氏筛 + 集合）、set_algebra（并 / 交 / 推导式 / 相等）、sort_into_set（去重排序）、bool_logic（布尔真值表） |
| 边界 | `tests/edge/*.l26` | 9 | comprehension、deep_nesting、division、empty_set、negatives、remove_absent、set_near_max、shadowing、union_inter |
| 错误 | `tests/errors/*.l26` | 10 | 重声明、未声明使用、类型不匹配、非集合做集合操作、`in` 右操作数非集合、缺分号、括号不配对等，须全部被拒绝 |

运行命令与结论：

```sh
make && bash tests/run_tests.sh
...
================================
TOTAL: 28   PASS: 28   FAIL: 0
All tests passed.
```

**全部 28 项测试通过**，覆盖：三类数据类型与 I/O、`if` / `while` 控制流、集合
全部操作、两项附加功能（集合相等判定、集合推导式）、作用域遮蔽、运行期与编译
期错误检测。三个具备完整逻辑功能的代表性程序（factorial、gcd、prime_sieve）
均编译运行且输出正确，满足「至少三个具备逻辑功能的程序」要求。
