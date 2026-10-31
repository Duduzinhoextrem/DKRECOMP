# Progresso

Somente o que foi executado e verificado. Cada linha aponta para o comando que a reproduz (`docs/TESTING.md`).

## 2026-10-31 — Primeiro boot: o SDK do jogo inicializa até a primeira mensagem IPC (MVP 5)

Novas peças: `tools/dkrecomp/link.py` (layout e ligação estática dos módulos, o trabalho do `rtld` feito na geração),
`dkrecomp.gen` para a ExeFS inteira (imagens ligadas, uma tabela de despacho por módulo, `layout.c`, um
`build.ninja`), descoberta de funções por ponto fixo (desvios diretos para fora da função, ponteiros relocados e
endereços calculados por `ADR`/`ADRP+ADD`), `runtime/src/loader.c` e `runtime/src/horizon.c` (espaço de endereços,
thread principal, callbacks do `rtld`, SVCs com log).

**Sequência de boot, lida do `rtld` do próprio jogo:** `nn::init::Start(handle_da_thread_principal,
endereço_logo_após_o_rtld, cb_flag, cb_inits)`. `cb_flag` grava 1 numa variável do `rtld`; `cb_inits` percorre a
lista de módulos chamando a inicialização de cada um. O SDK 4.4.0 passa 2 callbacks (o SDK do TotK, 3).

| Medida (ExeFS 1.0.0) | Resultado |
|---|---|
| Módulos | `main` @ `0x800000000` (100 MiB), `subsdk0` @ `0x806600000` (7 MiB), `sdk` @ `0x806e00000` (10,5 MiB) |
| Relocações | só `RELATIVE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT`; não resolvidos: só símbolos internos do `rtld` e um fraco opcional |
| Funções geradas | `main` 59.632, `subsdk0` 13.236, `sdk` 28.109 |
| Geração / compilação (12 processos) / executável | ~40 s / ~9 min / `dkctf.exe` de 106 MB |
| **Boot** | `nn::init::Start` roda; 18 SVCs atendidas (GetInfo de entropia, regiões, memória; QueryMemory; prioridade/ID da thread; SignalProcessWideKey; ConnectToNamedPort `"sm:"`); callback 1 do `rtld` chamado; **para na 19ª: primeira mensagem IPC ao `sm:`** (cabeçalho `00000004 8000000a 00000001…`, pedido com PID: `sm:Initialize`) |

Confirmado no boot: `x1` é a base do primeiro módulo (o SDK consulta exatamente `0x800000000` com QueryMemory).
Primeira tentativa: parou num salto de cauda para um stub de PLT não descoberto; levou à descoberta por ponto fixo.

## 2026-10-31 — O `main` inteiro do jogo compila e liga

Nova ferramenta `tools/dkrecomp/gen.py` (`python -m dkrecomp.gen`): gera o C de um módulo NSO inteiro, dividido em
arquivos, com a tabela de despacho e um `build.ninja`; `runtime/src/probe_main.c` liga um executável de sonda que
confere a tabela sem executar código do jogo.

| Medida (ExeFS 1.0.0, `main`, base provisória `0x800000000`) | Resultado |
|---|---|
| Funções geradas | 58.606 (57.776 com nome + 830 alvos de `BL` sem símbolo) |
| Instruções recompiladas | 2.987.362, **0 não suportadas** |
| Tempo de geração (Python, 1 processo) | 23,5 s |
| C gerado | 310 MB em 99 arquivos (maior: 7,9 MB) |
| Compilação (Clang do Zig, `-O1`, 12 processos, Ryzen 7 5700X) | 276 s, 0 erros; memória ~3 GB no total, ≤ 0,4 GB por processo |
| Objetos / executável de sonda | 439 MB / 65 MB |
| Sonda: tabela ordenada e cada entrada encontrável por `rt_lookup` | 58.606 entradas, 0 erros |
| Forma do código | 177.127 chamadas diretas, 37.249 chamadas por `rt_dispatch`, 5.725 funções com despacho local de `BR`; maior função com 29.862 linhas de C |

**O que isso prova e o que não prova:** o gerador aguenta o tamanho real e o C gerado é válido para o compilador.
Nenhuma linha do jogo foi executada: falta carregar o módulo na memória (segmentos, relocações, `.bss`), ligar com
o `sdk`, criar threads/TLS e implementar SVCs e serviços (MVP 5).

## 2026-10-31 — SIMD vetorial: `main` 100% coberto

Novo: `runtime/include/dkr/simd.h` e as classes vetoriais no decodificador/lifter:
- FP vetorial (FADD, FSUB, FMUL, FDIV, FMAX/FMIN(NM), FABD, FADDP/FMAXP/FMINP, FMULX, FMLA/FMLS, comparações
  com registrador e com zero, FABS, FNEG, FSQRT, conversões);
- estimativas FRECPE/FRSQRTE (algoritmo da Arm) e passos FRECPS/FRSQRTS;
- por elemento: FMUL/FMULX/FMLA/FMLS (vetor e escalar), MUL;
- inteiros: ADD, SUB, MUL, MLA/MLS, ADDP, comparações, CMTST, REV16/32/64, CNT, NOT, ABS, NEG, SHL/SSHR/USHR,
  SSHLL/USHLL(2), XTN(2), [SU]ADDL/SUBL/MLAL/MLSL/MULL(2), ADDV, ADDP escalar;
- BSL/BIT/BIF; ZIP/UZP/TRN; EXT; TBL/TBX; DUP e INS de elemento;
- MOVI/MVNI/ORR/BIC/FMOV vetoriais (todos os modos de imediato);
- LD1/ST1 de uma lane e LD1R, com pós-incremento;
- a palavra de armadilha `0xE7FFDEFE` do compilador da Nintendo, tratada como trap.

| Evidência | Resultado |
|---|---|
| `insn_diff --only vec_`, sementes 61–64 e 72, 6–8 mil instruções | 146.000 casos, 0 falhas |
| Teste de mutação SIMD: arredondamento do RecipEstimate, ZIP, NaN em comparação, SSHR sem sinal | as 4 detectadas (25, 438, 32, 32 casos) |
| `func_diff --vectorize` (programas vetorizados pelo Clang), O0/O1/O2/Os | 416 execuções, 0 falhas (Unicorn = recompilado = nativo) |
| `func_diff` sem vetorização, regressão inteira (sementes 11–12) e FP (semente 33) | 0 falhas |
| `pytest` | 39 testes passam |

Bug encontrado: `FRSQRTS` estourava para infinito perto do limite do float, porque 3 − a·b era arredondado antes
da divisão por 2. Corrigido dividindo um operando normal por 2 antes (exato), mantendo um só arredondamento.

**Cobertura no jogo (1.0.0)**:

| Módulo | Funções 100% suportadas | Instruções suportadas |
|---|---|---|
| `main` | 99,5% → **100%** (57.776 / 57.776) | 99,89% → **100%** |
| `subsdk0` | 99,9% → **100%** | **100%** |
| `sdk` | 99,3% → 99,6% (67 funções faltando) | 99,79% |

O que resta é só no `sdk`: AES, SHA-1, SHA-256, PMULL, CRC32, `CNTPCT_EL0`, `CTR_EL0` e algumas formas
saturadas (`sqadd`, `sqrshrn`).

## 2026-10-30 (noite) — Ponto flutuante escalar

Novo: `runtime/include/dkr/fp.h` e regras no decodificador/lifter para FP escalar (single e double): aritmética
(FADD, FSUB, FMUL, FDIV, FNMUL, FMAX/FMIN/FMAXNM/FMINNM, FMADD/FMSUB/FNMADD/FNMSUB fundidos), FABS, FNEG, FSQRT,
FMOV (registrador e imediato), FCVT entre single e double, FRINT N/P/M/Z/A/X/I, FCMP/FCMPE, FCCMP/FCCMPE, FCSEL,
conversões FP↔inteiro em todos os modos de arredondamento (com saturação) e em ponto fixo, SCVTF/UCVTF escalares
SIMD.

| Evidência | Resultado |
|---|---|
| `insn_diff --only fp_`, sementes 31–33, 8.000 instr. × 4 estados | 96.000 casos, 0 falhas |
| `insn_diff --only fp_`, semente 51 (com ponto fixo), 6.000 × 4 | 24.000 casos, 0 falhas |
| Teste de mutação FP: NaN padrão, prioridade de SNaN, saturação, FMA não fundido | as 4 detectadas (108, 13, 37 e 19 casos) |
| `func_diff` O0/O1/O2/Os com `fp_physics` (float/double, sqrt, floor, ponto fixo) | 416 execuções, 0 falhas (Unicorn = recompilado = nativo) |
| `insn_diff` sementes 11–12 (regressão do conjunto inteiro) | 59.900 casos, 0 falhas |
| `pytest` | 34 testes passam |

**Cobertura no jogo (1.0.0)**, `dkrecomp.coverage`:

| Módulo | Funções 100% suportadas | Instruções suportadas |
|---|---|---|
| `main` | 90,8% → **99,5%** (57.489 / 57.776) | 97,32% → **99,89%** |
| `sdk` | 97,2% → 99,3% | 98,74% → 99,65% |
| `subsdk0` | 97,9% → 99,9% | 99,76% → 99,997% |

O que resta no `main` (287 funções): SIMD vetorial — `fmul`/`fadd` em vetores, `frecpe`/`frecps`/`frsqrte`/`frsqrts`,
`dup`/`mov` de elemento, `sshll`, `tbl`, `ld1`/`st1`, `zip`, `ext`, comparações vetoriais.

Bug encontrado pelo teste de função: nenhum no lifter; o Clang usa conversões em ponto fixo (`ucvtf s0, w8, #6`)
para divisões por potência de dois, que foram então implementadas.

## 2026-10-30 (tarde) — ExeFS 1.0.0 analisada (MVP 2 concluído)

O usuário extraiu a ExeFS e o `control.nacp` do jogo base com as próprias ferramentas e chaves, em
`SwitchExtract/` e `user_files/` (ambas ignoradas pelo Git). Relatórios em `reports/identify_exefs_v0.*` e
`reports/coverage_v0.*` (ignorados pelo Git).

| Resultado | Evidência |
|---|---|
| Base = versão 1.0.0 | `control.nacp` |
| ExeFS íntegra: hashes dos segmentos de `main`, `sdk`, `subsdk0`, `rtld` conferem | `dkrecomp.identify user_files/exefs` |
| SDK 4.4.0, NVN 4.4.0, motor da Retro Studios, Scaleform, NEX 4.2.1 | strings e símbolos |
| `main` tem 57.776 funções com nome e tamanho (96,3% do código) | `.dynsym` |
| Decodificador aceita 97,32% das instruções do `main`; 90,8% das funções inteiras | `dkrecomp.coverage user_files/exefs` |
| O que falta é quase todo FP escalar; no `sdk` também AES/SHA, `CNTPCT_EL0`, `CTR_EL0`, CRC32 | idem |

Marcos: **MVP 2 concluído** (2a NSPs, 2b ExeFS, 2c cobertura). Nova ferramenta: `tools/dkrecomp/coverage.py`.

## 2026-10-30 — Fase 1 a 4 iniciais

### Estado dos marcos

| Marco | Estado | Evidência |
|---|---|---|
| MVP 1 — Ferramentas | **Concluído** | `pytest`: 29 testes passam; `ctest`: `dkr_selftest` passa (build CMake + Ninja + Clang) |
| MVP 2 — Análise | **Parcial** (2a concluído; 2b/2c bloqueados) | `identify --hash` nos dois NSPs: título, versão e SHA-256 de 12/12 conteúdos conferem |
| MVP 3 — Recompilação | **Concluído para o subconjunto suportado** | `func_diff`: funções ARM64 recompiladas = Unicorn = C nativo, em O0/O1/O2/Os; `insn_diff` sem falhas |
| MVP 4 — Integração | **Concluído (escopo de teste)** | `func_diff`: despacho indireto pela tabela do runtime, atômicos via `rt_cas`, `SVC #0x26` entregue ao `dkr_svc_hook` e retomado, memória mapeada por `rt_map_fixed` |
| MVP 5 — Primeiro boot | Não iniciado | Bloqueado por B1 (ExeFS) e pela falta de aritmética SIMD/FP |
| MVP 6 — Gameplay | Não iniciado | — |

### Identificação dos arquivos do usuário (MVP 2a)

`dkrecomp.identify "game original meu" --hash`:

| NSP | Tipo | Title ID | Versão | Conteúdos | SHA-256 |
|---|---|---|---|---|---|
| `[0100C1F0051B6000][v0].nsp` (6,6 GiB) | Application | `0100c1f0051b6000` | 0 | Program, Control, LegalInformation, Meta | 4/4 conferem |
| `[0100C1F0051B6800][v131072].nsp` (43,2 MiB) | Patch | `0100c1f0051b6800` | 131072 (2ª atualização) | 4 DeltaFragment, Program, Control, LegalInformation, Meta | 8/8 conferem |

Tickets: *common*, key generation 4, rights id = title id. Valores registrados em `titles/dkctf/title.toml`.

### Recompilador

Operações suportadas (decodificador + lifter), todas cobertas pelos testes diferenciais:

- Inteiros: ADD/SUB/ADDS/SUBS (imediato, registrador deslocado, estendido), ADC/SBC(S), AND/ORR/EOR/ANDS
  (imediato e registrador, com inversão BIC/ORN/EON/BICS), MOVZ/MOVN/MOVK, ADR/ADRP, SBFM/BFM/UBFM, EXTR,
  MADD/MSUB, SMADDL/UMADDL/SMSUBL/UMSUBL, SMULH/UMULH, UDIV/SDIV, LSLV/LSRV/ASRV/RORV, CSEL/CSINC/CSINV/CSNEG,
  CCMP/CCMN, CLZ/CLS/RBIT/REV/REV16/REV32.
- Desvios: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET; SVC e BRK (para o runtime).
- Memória: LDR/STR de 8–64 bits com e sem sinal (offset, pré/pós-indexado, sem escala, registrador estendido),
  LDP/STP/LDPSW, LDR literal, LDAR/STLR, LDXR/LDAXR/STXR/STLXR, PRFM (ignorado), DMB/DSB/ISB, CLREX.
- Sistema: MRS/MSR de NZCV, TPIDR_EL0, TPIDRRO_EL0 (leitura), FPCR, FPSR; MRS de CNTVCT/CNTFRQ/DCZID.
- SIMD/FP (só movimentação): LDR/STR/LDP/STP de B/H/S/D/Q (todas as formas de endereço), LDR literal, FMOV entre
  registradores gerais e SIMD (W↔S, X↔D, X↔V.D[1]), INS (geral), UMOV, DUP (geral e escalar), MOVI 64-bit,
  AND/BIC/ORR/ORN/EOR vetoriais.

### Testes diferenciais

| Teste | Volume | Resultado |
|---|---|---|
| `insn_diff` sementes 1–4 (antes do SIMD), 8.000 instruções × 3 estados | 95.857 casos | 0 falhas |
| `insn_diff` sementes 11–14 (com SIMD), 10.000 instruções × 3 estados | 119.751 casos, 68 tipos de operação | 0 falhas |
| `func_diff` O0/O1/O2/Os, 8 sementes × 12 testes (inclui SVC) | 384 execuções | 0 falhas (Unicorn = recompilado = nativo) |
| `pytest` (unitários + diferenciais reduzidos) | 29 testes | 0 falhas |
| `ctest` (`dkr_selftest`, valores conhecidos do `cpu.h`) | 1 teste, ~40 verificações | 0 falhas |
| Teste de mutação (2 bugs plantados) | — | ambos detectados |

### Bugs encontrados pelos testes e corrigidos

1. **LDR literal lia o endereço errado**: o campo do endereço literal se chamava `addr` e colidia com o endereço da
   instrução. Detectado por `insn_diff` (2 casos); renomeado para `lit`.
2. **LDAR/STLR com `o0=0` aceitos**: são LDLAR/STLLR (ARMv8.1), inexistentes no Switch. O Unicorn (A72) os rejeita;
   o decodificador agora também.
3. **Sobreposições CONSTRAINED UNPREDICTABLE aceitas** (ex.: `ldp x0, x0`): detectadas comparando com o Capstone;
   agora rejeitadas.
4. **Mapeamento fixo falhava fora de 64 KiB** e **commit atravessando reservas** (erro 487): corrigidos em
   `rt_map_fixed`.
5. Falha de teste que **não era** bug do lifter: o monitor exclusivo do Unicorn vazava entre casos (1 em ~60 mil).
   Confirmado executando `CLREX` antes de cada caso.
6. **Laço infinito no modo de teste passo-a-passo**: um desvio para a própria instrução (`b .`) virava `goto` para
   si mesmo, enquanto o Unicorn executa um passo. Corrigido no modo `pc` do lifter; o runner agora tem tempo-limite.
   O modo normal de recompilação não era afetado.

### Ambiente

Ferramentas instaladas só no `.venv` do projeto (`requirements-dev.txt`). Nada global foi alterado. Referências em
`refs/` (somente leitura, ignoradas pelo Git).

### Próximo passo

1. Usuário fornece a ExeFS (e o `control.nacp`) extraídos da versão alvo → rodar `identify` na ExeFS e medir a
   cobertura do decodificador sobre o `main` real (MVP 2b/2c).
2. Aritmética SIMD/FP no lifter, com testes diferenciais (decisão de reutilizar ou não o código do TotK/LAS).
3. Descoberta de funções em NSO sem símbolos e loader NSO com relocações (pré-requisitos do MVP 5).
