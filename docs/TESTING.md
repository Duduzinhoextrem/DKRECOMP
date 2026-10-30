# Testes

Nenhum teste precisa dos arquivos do jogo, exceto a identificação dos NSPs do usuário.

## Preparar o ambiente (uma vez)

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

Todos os pacotes vêm do PyPI e ficam no `.venv` do projeto; nada é instalado no sistema. `ziglang` traz o Clang/LLD
usado para compilar o C gerado (x86-64) e os programas de teste (ARM64).

## Rodar tudo

```bash
.venv/Scripts/python -m pytest
```

Inclui testes unitários (formatos, decodificador × Capstone) e versões reduzidas dos testes diferenciais.

## Testes diferenciais

### Instrução isolada (`tests/diff/insn_diff.py`)

Gera instruções aleatórias a partir de modelos de codificação do manual ARM, mantém as que o decodificador aceita,
gera estados de registradores (valores aleatórios e de borda) e executa cada uma:
- no **Unicorn** (modelo Cortex-A72, ARMv8.0 como o Cortex-A57 do Switch);
- como **C recompilado** (`tests/diff/harness.c`), com a memória mapeada nos mesmos endereços.

Compara X0–X30, SP, NZCV, TPIDR, PC, V0–V31 e os 4 KiB de memória de rascunho.

```bash
.venv/Scripts/python tests/diff/insn_diff.py --seed 11 --count 10000 --states 3
```

### Funções inteiras (`tests/diff/func_diff.py`)

`tests/diff/cases/programs.c` (recursão, laços, ordenação, bytes com sinal, `switch` com tabela de salto,
multiplicação de 128 bits, ponteiros de função, cópia de structs, atômicos LDAXR/STLXR, seleção condicional, `SVC`
entregue a um tratador do runtime) é
compilado pelo Clang para ARM64. O DKRecomp recompila todas as funções do ELF, e `test_entry(which, seed)` roda de
três formas que precisam concordar: **Unicorn = recompilado = mesmo C compilado nativamente para x86-64**. Recompilado
e Unicorn também precisam concordar em todos os registradores e no `.data/.bss`.

```bash
.venv/Scripts/python tests/diff/func_diff.py --opt O2,O1,O0,Os --seeds 8
```

O Clang é chamado com `-fno-vectorize -fno-slp-vectorize` porque aritmética SIMD ainda não é suportada.

### Teste de mutação (validação dos próprios testes)

Feito manualmente em 2026-10-30 numa cópia do projeto: dois bugs plantados no lifter foram detectados.

| Mutação | `insn_diff` (1.500 instr.) | `func_diff` (O2) |
|---|---|---|
| Carry de `AddWithCarry` errado quando `r == a` | 5 casos falharam | não detectou (caso de borda raro) |
| `CSINC` somando 2 em vez de 1 | 8 casos falharam | 4 execuções falharam |

## Runtime via CMake

```bash
.venv/Scripts/cmake -S . -B build/zig -G Ninja -DCMAKE_MAKE_PROGRAM=.venv/Scripts/ninja.exe -DCMAKE_TOOLCHAIN_FILE=tools/toolchain/zig-x86_64-windows.cmake -DCMAKE_BUILD_TYPE=Release
```

```bash
.venv/Scripts/cmake --build build/zig
```

```bash
.venv/Scripts/ctest --test-dir build/zig --output-on-failure
```

`dkr_selftest` verifica valores conhecidos dos auxiliares do `cpu.h` (divisão, flags, condições, CLZ/CLS, REV,
multiplicação alta).

## Identificação dos arquivos do usuário

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify "game original meu" --hash --out reports/identify_nsp
```

Lê ~6,7 GB para conferir os SHA-256 (cerca de 3,5 minutos a ~35 MiB/s nesta máquina).

## Análise da ExeFS extraída pelo usuário

```bash
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify user_files/exefs --out reports/identify_exefs_v0
```

```bash
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.coverage user_files/exefs --out reports/coverage_v0
```

O segundo mede quantas instruções de cada módulo o decodificador aceita (cerca de 20 s). Repetir depois de cada
avanço do lifter.

## Resultados registrados

Ver `docs/PROGRESS.md`. Regra: resultado só entra na documentação se foi produzido por um comando acima.
