# Roadmap

Marcos com critérios de aceitação verificáveis. Um marco só é declarado concluído com a evidência listada.
Estado atual de cada um: `docs/PROGRESS.md`.

## MVP 1 — Ferramentas

**Critério**: o projeto compila e executa testes independentes do jogo, com um comando, nesta máquina.
**Evidência exigida**: `pytest` verde; `ctest` verde no build CMake.
**Estado**: concluído (2026-10-30).

## MVP 2 — Análise

**Critério**: ler e identificar os arquivos fornecidos pelo usuário, sem erros nem interpretações inventadas.
- 2a. NSPs: título, versão, tipo e integridade de cada conteúdo. **Concluído** (SHA-256 de 12/12 conteúdos confere).
- 2b. ExeFS extraída: build id, segmentos com hash, imports/exports, NPDM (serviços, SVCs), middleware, contagem de
  SVCs. **Concluído** (2026-10-30, ExeFS 1.0.0).
- 2c. Relatório de cobertura do lifter sobre o `main` real. **Concluído**: 97,32% das instruções e 90,8% das
  funções do `main` já suportadas (`dkrecomp.coverage`).

## MVP 3 — Recompilação

**Critério**: uma rotina ARM64 de teste é recompilada e executada corretamente no Windows.
**Evidência exigida**: `tests/diff/func_diff.py` — funções compiladas para ARM64, recompiladas, com resultado igual ao
Unicorn e ao C nativo, em vários níveis de otimização; `tests/diff/insn_diff.py` sem falhas.
**Estado**: concluído (2026-10-30); desde 2026-10-31 o decodificador cobre 100% das instruções do `main` do jogo.

Próximos passos dentro da Fase 4 (ordem guiada pela cobertura medida no jogo):
1. ~~FP escalar~~ **feito** (2026-10-30): `main` em 99,89% das instruções e 99,5% das funções.
2. ~~SIMD vetorial do `main`~~ **feito** (2026-10-31): `main` e `subsdk0` 100%. Resta no `sdk` (67 funções):
   AES/SHA/PMULL, CRC32, `CNTPCT_EL0`, `CTR_EL0`, formas saturadas. Antes, a lista era: `fmul`/`fadd` vetoriais, `frecpe`/`frecps`/`frsqrte`/`frsqrts`
   (estimativas com tabela da Arm — precisam ser bit-exatas), `dup`/`mov` de elemento, `sshll`, `tbl`, `ld1`/`st1`,
   `zip`, `ext`, comparações vetoriais. No `sdk`: AES/SHA, CRC32, `CNTPCT_EL0`, `CTR_EL0`.
3. Descoberta de funções para `sdk`/`subsdk0` (os exports cobrem só ~65% do código deles): alvos de `BL`,
   `.eh_frame_hdr`, relocações, como o `gen.py` do TotK. O `main` já tem símbolos para 96,3%.
4. Resolução estática de tabelas de salto (hoje: `switch` local, correto mas grande).
5. Escala: geração paralela e particionada em vários `.c`; flags e registradores em variáveis locais.

## MVP 4 — Integração

**Critério**: um módulo de teste interage com o runtime: chamadas indiretas pela tabela de despacho, atômicos,
SVCs tratados por um tratador do host e memória mapeada pelo runtime.
**Estado**: concluído no escopo de teste (2026-10-30). O `func_diff` exercita despacho indireto (`BLR` por ponteiro
de função), atômicos via `rt_cas` e um `SVC #0x26` entregue a `dkr_svc_hook` e retomado, com o mesmo tratador
aplicado no Unicorn por um hook de interrupção. Os SVCs reais do Horizon pertencem ao MVP 5.

## MVP 5 — Primeiro boot

**Estado (2026-10-31): tentativa verificável alcançada.** O `dkctf.exe` carrega os três módulos ligados, chama
`nn::init::Start` e o SDK inicializa com 18 SVCs atendidas até a primeira mensagem IPC (ao `sm:`), onde para com
relatório. Próximos passos, nesta ordem:
1. IPC do Horizon (HIPC/CMIF): ler e escrever mensagens no TLR, descritores de handle, PID e buffers.
2. `sm:` (Initialize, GetService) e um registro de serviços do host, implementados um a um conforme o log pedir.
3. Threads (CreateThread/StartThread) com threads do host, sincronização real (mutex, condição, eventos) e o
   modelo de núcleos/prioridades.
4. Os callbacks de inicialização dos módulos (`DT_INIT`) e a chegada ao `nnMain`.
5. Sistema de arquivos (RomFS combinada) e saves.

Critério original:

**Critério**: tentativa verificável de iniciar o jogo real: o `main` recompilado roda a partir do ponto de entrada,
com log de cada SVC/serviço até o primeiro ponto em que falta implementação.
**Pré-requisitos**: ~~MVP 2b~~, ~~cobertura SIMD/FP~~, ~~gerar e compilar o `main` inteiro~~ (feitos em
2026-10-31); falta: loader NSO no runtime (mapear segmentos no endereço base, aplicar relocações `R_AARCH64_RELATIVE`
e as de símbolo, zerar `.bss`), layout definitivo dos módulos (hoje base provisória `0x800000000`), gerar e compilar
`sdk`/`subsdk0` (com descoberta de funções além dos exports e o que falta de cripto/CRC), ligação entre módulos por
símbolo no lugar do `rtld`, TLS/TLR por thread, SVCs básicos (memória, threads, sincronização, tempo), `sm:` e os
primeiros serviços. Nenhum gráfico ainda.

## MVP 6 — Gameplay

**Critério**: alcançar gameplay controlável, se os problemas técnicos forem resolvidos.
**Pré-requisitos**: NVN → Vulkan (avaliar reutilização do back-end do TotK e do tradutor de shaders do Ryujinx),
áudio (`audren:u` + SDL3), entrada (HID → SDL3), sistema de arquivos (RomFS + saves), modelo de núcleos/prioridades.
Somente depois: resolução configurável, 60 FPS, frame pacing — e só com medições.

## Depois

- Verificador automático de conteúdo proibido antes de commits (como `check_content.py` do TotK).
- *Donkey Kong Country Returns HD* como alvo separado (`titles/dkcr_hd/`), com análise própria.
