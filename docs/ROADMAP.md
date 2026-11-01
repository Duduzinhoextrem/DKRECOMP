# Roadmap

Marcos com critérios de aceitação verificáveis. Um marco só é declarado concluído com a evidência listada.
Estado atual de cada um: `docs/PROGRESS.md`.

**Checkpoint 2026-11-01:** boot em 1.200 presents/30 s, saves persistentes e áudio CPU/manual processando
quadros silenciosos. Comandos preservam dados transitórios. Os 8 shaders de inicialização e 3.460 únicos de
materiais já traduzem e passam no validador Vulkan 1.3. O trabalho restante é integrar replay/recursos/shaders,
mostrar o título real, publicar controles e validar uma fase. **MVP de gameplay continua pendente.**

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
3. **Descoberta implementada**: alvos de desvios, `.eh_frame_hdr`, relocações e ADR/ADRP+ADD complementam os
   símbolos, inclusive para callbacks indiretos. Validar novos destinos conforme aparecerem no boot.
4. Resolução estática de tabelas de salto (hoje: `switch` local, correto mas grande).
5. Escala: geração paralela e particionada em vários `.c`; flags e registradores em variáveis locais.

## MVP 4 — Integração

**Critério**: um módulo de teste interage com o runtime: chamadas indiretas pela tabela de despacho, atômicos,
SVCs tratados por um tratador do host e memória mapeada pelo runtime.
**Estado**: concluído no escopo de teste (2026-10-30). O `func_diff` exercita despacho indireto (`BLR` por ponteiro
de função), atômicos via `rt_cas` e um `SVC #0x26` entregue a `dkr_svc_hook` e retomado, com o mesmo tratador
aplicado no Unicorn por um hook de interrupção. Os SVCs reais do Horizon pertencem ao MVP 5.

## MVP 5 — Primeiro boot

**Estado (2026-10-31): tentativa verificável alcançada; `nnMain` é chamado.** O `dkctf.exe` carrega os três módulos
ligados, roda os construtores e inicializa o SDK. Há uma segunda thread em execução e leituras reais da RomFS.
Continuar guiado pelo primeiro comando ou salto pendente do log:
1. ~~IPC do Horizon (HIPC/CMIF)~~ **feito** (2026-10-31), com domínios e mensagens de controle.
2. ~~`sm:` e registro de serviços~~ **feito**; os serviços iniciais já atendem o boot. Continua conforme o log pedir.
3. **Threads/sincronização implementadas e testadas** (2026-10-31): CreateThread/StartThread, mutex, condição,
   eventos e espera pela saída. A segunda thread já executa no boot. Modelo de núcleos/prioridades ainda pendente.
4. Os callbacks de inicialização dos módulos (`DT_INIT`) **rodam e retornam**; a entrada em **`nnMain` é registrada**.
5. **RomFS base 1.0.0 extraída e reempacotada**: 83 arquivos, parser independente aceita a imagem. IStorage
   validado com dados sintéticos e a leitura do header real no boot; implementar saves e demais comandos conforme o log.
6. **Usuário pré-selecionado implementado e validado** (`PopLaunchParameter(2)`): boot chega a 267 SVCs.
7. **Montagem do save de conta implementada** (`fsp-srv` 51); IFileSystem/IFile ainda sem operações de arquivo.
8. **Fronteira gráfica do host ligada** com overrides e lookup NVN: cria contexto Vulkan real, sem frames do jogo.
   **Memória compartilhada/HID inicial implementados:** SDK mapeia o bloco de 256 KiB com proteção de leitura,
   usando aliases reais do Windows. Ativação/configuração de Npad e lista de vibração passam no boot; rings ainda
   vazios, back-end de entrada pendente. **VI inicial implementado:** display e parcel NativeWindow aceitos pelo SDK,
   Binder com refs/evento rastreados. Connect/Disconnect e escala implementados; Connect do SDK passa.
   **Inicialização gráfica NVN completa** (2026-10-31): layouts medidos no driver NVN original do `sdk`, 211 procs
   puros rodam o código original, o resto no host. Comandos são gravados mas não desenhados (sem replay/shaders).
   Boot gráfico medido em **2876 SVCs**, parando em **`audren:u`**. Modo legado revalidado em `nvdrv` (292 SVCs).
9. **Áudio CPU/manual e save ligados**: renderer abre e processa quadros silenciosos; save cria/grava/confirma/lê.
   Com decoder de filmes indisponível e ZCull sem armazenamento externo, boot alcançou **1.200 presents/30 s**,
   sem STOP/exceção. **Próximo:** captura completa dos comandos, replay Vulkan e tradução dos shaders.

Critério original:

**Critério**: tentativa verificável de iniciar o jogo real: o `main` recompilado roda a partir do ponto de entrada,
com log de cada SVC/serviço até o primeiro ponto em que falta implementação.
**Pré-requisitos**: ~~MVP 2b~~, ~~cobertura SIMD/FP~~, ~~gerar e compilar o `main` inteiro~~ (feitos em
2026-10-31). Loader, layout dos módulos, relocações, descoberta, compilação de sdk/subsdk0, ligação estática,
TLR por thread, SVCs básicos e os primeiros serviços também estão implementados. A continuação depende dos
comandos restantes, instruções ainda não suportadas se executadas e demais subsistemas. Nenhum gráfico ainda.

## MVP 6 — Gameplay

O servidor CPU/manual do SDK substitui a proposta inicial de simular áudio silencioso por IPC. Abertura e
processamento de 200 quadros silenciosos medidos; não há áudio audível confirmado, controles publicados ou
imagem do jogo. O critério da Etapa B foi atingido com 1.200 presents, sem renderização; Etapa C é a próxima.

Avaliação da integração gráfica: `docs/GRAPHICS_PORT.md`. O back-end próprio do host passa no teste de
clear/apresentação, resize e readback, inclusive validação de sincronização. Ainda não há frame do jogo nem NVN.

**Critério**: alcançar gameplay controlável, se os problemas técnicos forem resolvidos.
**Pré-requisitos**: NVN → Vulkan (avaliar reutilização do back-end do TotK e do tradutor de shaders do Ryujinx),
áudio (`audren:u` + SDL3), entrada (HID → SDL3), sistema de arquivos (RomFS + saves), modelo de núcleos/prioridades.
Somente depois: resolução configurável, 60 FPS, frame pacing — e só com medições.

## Depois

- Verificador automático de conteúdo proibido antes de commits (como `check_content.py` do TotK).
- *Donkey Kong Country Returns HD* como alvo separado (`titles/dkcr_hd/`), com análise própria.
