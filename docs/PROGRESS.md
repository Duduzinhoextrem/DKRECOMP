# Progresso

Somente o que foi executado e verificado. Cada linha aponta para o comando que a reproduz (`docs/TESTING.md`).

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
