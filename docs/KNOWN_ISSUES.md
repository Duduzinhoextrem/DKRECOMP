# Problemas conhecidos

## Prioridades medidas em 2026-11-03

- **Vídeos:** tocam pelo Media Foundation (`runtime/src/movie.c`), só no modo CPU com saída NV12 e áudio AAC. O modo de textura NVN (mode 2) e outros codecs são recusados com log. Ainda não testados: `outro.mp4`, `dkBand.mp4` e os `Demo1–4` da tela de título.
- **Launcher/configurações:** a escala de resolução foi verificada só nos menus; texturas que o jogo lê com
  `texelFetch` sem passar pelo tradutor (bindless, arrays) não são escaladas, e um draw que mistura alvos de escalas
  diferentes é registrado no log. FPS acima de 60 só foi medido nos menus. A preparação completa a partir do NSP
  (RomFS + compilação) ainda não foi executada de ponta a ponta pelo launcher.
- **FPS dos vídeos:** os MP4 são 29,97 fps; interpolação temporal em tempo real para 60 fps (`movie_smoothing = true` por padrão, SWAR 64 bits com detecção de corte de cena em `runtime/include/dkr/movie_interp.h`) está ativa.
- **Transform feedback:** implementado via `VK_EXT_transform_feedback` (`runtime/gfx/vk_draw.c` e `runtime/gfx/nvn_replay.c`).
- **Cópias de vértices:** ~20 MB/frame ainda são copiados da memória do guest a cada batch. Mapear os memory pools com
  `VK_EXT_external_memory_host` eliminaria a cópia.
- **CPU:** C gerado em `-O1`; `rt_dispatch` faz busca binária a cada chamada indireta. Hoje não limita os menus (a thread
  do jogo fica ociosa), mas deve pesar na gameplay.
- **Áudio:** saída principal em WASAPI shared event-driven (`runtime/src/audio_sdk.c`) com fila alvo de 15 ms (720 amostras), cadência de 5 ms (240 amostras) e sinalização do evento de kernel `g_audio_event` a cada quadro; fallback em WinMM quando WASAPI não estiver disponível.

## Bloqueios

| # | Problema | Impacto | Saída |
|---|---|---|---|
| B1 | Extração dos arquivos proprietários | Resolvido para o alvo **base 1.0.0**: ExeFS e RomFS disponíveis localmente | Outras versões exigem seus próprios arquivos extraídos; DKRecomp não decripta |
| B2 | O Visual Studio 2026 desta máquina não tem o Windows SDK (falta `kernel32.lib` e os headers da UCRT) | MSVC não compila nem liga; a compatibilidade MSVC do C gerado não foi verificada | Instalar o componente "Windows 11 SDK" pelo Visual Studio Installer (mudança no sistema: **requer autorização do usuário**). Enquanto isso, Clang do Zig |

## Limitações do recompilador

- O decodificador cobre 100% do `main` e do `subsdk0`. **No `sdk` faltam** as extensões de criptografia (AES,
  SHA-1, SHA-256, PMULL), CRC32 e outras formas fora do subconjunto. A contagem antiga de 67 funções antecede
  o suporte a SQADD/UQADD e narrowing saturado usado pelo áudio; precisa de nova medição de cobertura.
- Os auxiliares SIMD (`simd.h`) usam laços genéricos por elemento: corretos, mas lentos. Especializar por tamanho de
  elemento é trabalho de desempenho para depois do primeiro boot.
- **Só FPCR = 0 é modelado**: arredondamento ao mais próximo, sem flush-to-zero e sem modo default-NaN. Se o jogo
  escrever outro valor em FPCR, o resultado de FP pode divergir. Ainda não há aviso em tempo de execução para isso.
  As flags acumuladas de exceção do FPSR não são mantidas; FPSR.QC das novas operações inteiras saturadas é mantido.
- O C gerado exige **CPU com FMA** (`-mfma`, Haswell/Zen ou mais novo), porque o FMADD precisa de multiplicação-soma
  fundida exata. É o mesmo requisito do TotK Recomp e do LAS Recomp.
- Não suportados também: `DC ZVA` (o `DCZID_EL0` informa proibido), CRC32, LSE (ARMv8.1, não esperado no Switch),
  pares exclusivos (`LDXP/STXP`), `CAS`, instruções de sistema além de NZCV/TPIDR/TPIDRRO/FPCR/FPSR/CNTVCT/CNTFRQ.
- Sobreposições de registradores CONSTRAINED UNPREDICTABLE em loads/stores (ex.: `ldp x0, x0`) são rejeijadas de
  propósito.
- `RET` é tratado como retorno da função C, e `BL` assume que o chamado retorna ao LR. Código que manipula o LR para
  retornar a outro lugar (raro, ex.: `longjmp`) não é suportado ainda.
- `BR` com alvo desconhecido dentro da função usa um `switch` sobre todos os endereços da função: correto, mas gera
  muito código. Falta resolver tabelas de salto estaticamente.
- A descoberta de funções NSO combina símbolos, desvios, ponteiros relocados, endereços materializados e unwind.
  Os limites de funções sem símbolo são inferidos e podem ser imprecisos.
- O leitor de `.eh_frame_hdr` aceita o formato observado nos três módulos: versão 1, ponteiro pcrel/sdata4,
  contagem udata4 e tabela datarel/sdata4. Outros encodings geram erro explícito na geração.
- O monitor exclusivo guarda um endereço por CPU e usa compare-and-swap no `STXR`; isso tem o problema ABA em
  teoria (o mesmo do TotK/LAS). O teste de mutex com várias threads exercita CAS do runtime, mas não prova ausência de ABA no código guest.
- O contador `CNTVCT_EL0` usa 19,2 MHz (frequência do contador do Switch documentada no switchbrew), escalado do
  `QueryPerformanceCounter`; não foi comparado com hardware.

## Boot (runtime/src/horizon.c)

- Threads guest executam em paralelo em threads do Windows; o kernel serializa as SVCs. Não há escalonador
  de núcleos virtuais, preempção por prioridades Horizon nem herança de prioridade. A seleção dos waiters de
  mutex/condição respeita prioridade e FIFO, mas isso não equivale ao escalonador do console.
- Handles de threads e suas regiões locais ainda não são reciclados; há limite de 256 threads por execução.
  CancelSynchronization, suspensão/contexto de thread e arbitragem por endereço ainda não estão completos.
- Transfer memory registra o backing guest, tamanho e permissão, mas ainda não altera proteção de páginas nem
  os atributos apresentados por QueryMemory; MapTransferMemory/UnmapTransferMemory não foram implementados.
- Shared memory usa aliases reais e permissões de página. O alocador de placeholders aceita várias views em uma
  reserva, mas não substitui reservas privadas anteriores nem atravessa duas reservas de placeholders distintas.
  As sobras de reserva compartilhada também não podem ser commitadas por `rt_map_fixed`; isso exige unificar os
  alocadores se um futuro endereço escolhido pelo SDK usar esse caso. Falhas retornam erro, sem cópia de dados.
- O espaço de endereços (regiões de heap, alias, pilha, ASLR) e o tamanho de memória informados ao jogo foram
  escolhidos pelo DKRecomp; a memória total (`0xCD500000`) é uma aproximação.
- A entropia de `GetInfo(11)` é fixa, para que as execuções sejam reproduzíveis.
- A ordem de busca de símbolos na ligação (main, subsdk0, sdk; primeira definição global vence) segue o ELF padrão;
  que o `rtld` da Nintendo faça igual é hipótese.
- Funções descobertas por ponteiros podem começar em dados (algumas palavras inválidas no `subsdk0`/`main`); só
  causam erro se executadas.

## Serviços (runtime/src/services.c)

- No modo gráfico, o áudio usa o servidor CPU/manual recompilado do SDK 4.4. O boot abriu renderer e saída WASAPI/WinMM,
  com sinalização do evento de kernel por quadro de 5 ms. Outros SDKs/layouts e 32 kHz não são suportados nesse caminho.
  O modo IPC legado continua implementando apenas GetWorkBufferSize REV1–4.
- VI expõe somente Default 1280×720. Há limite de 64 layers; as identidades gerenciadas ainda duram até o fim
  do processo. Abertura/fechamento, parcel, refcounts e evento de disponibilidade não implementam compositor,
  transações de buffers, VSync ou o comportamento completo de Binder. Connect/Disconnect aceitam somente listener
  NULL e APIs Android 1–4; o parser de parcels só trata a interface do produtor. System/manager display param no primeiro
  comando próprio ainda ausente; devolver a interface não significa implementar todos os seus métodos.
  No modo legado `--no-gfx`, continua parando em `nvdrv:Initialize` (3).
- Cada serviço implementa só os comandos que o boot pediu (e vizinhos óbvios); o resto para com relatório. Os
  valores devolvidos (usuário local "DKRecomp", idioma `en-US`, região EUA, modo TV, foco, fuso UTC, configuração de
  desempenho) são escolhas do DKRecomp, não lidas de um console.
- Os ids de comando vêm do switchbrew para o firmware 4.x; um id errado aparece no log como comando inesperado.
- `QueryPointerBufferSize` responde 0x8000 para todo serviço; respostas por buffer de ponteiro (C) ainda não são
  escritas pelo IPC, só pelos serviços que as usam diretamente.
- O filesystem implementa a RomFS somente leitura (mapeada em memória via `MapViewOfFile`) e operações básicas de arquivos do save de conta. Enumeração
  de diretórios, renomeação, remoção recursiva, SD card e outros tipos ainda faltam. Commit faz flush, sem journal
  transacional; sem quota própria de save e com regras de nomes/case do Windows. A raiz suporta caminhos locais de drive;
  UNC e reparse points são recusados. A imagem tem que ser regenerada se os arquivos extraídos mudarem; ela não é o NCA original nem contém
  suas camadas de integridade. A abertura verifica tamanho e header, sem auditoria completa de todos os metadados.

## Módulo gerado

- O endereço base do `main` (`0x800000000`) é **provisório** e fica embutido no C gerado; mudar o layout exige
  regerar e recompilar (~5 minutos).
- 5.725 funções usam o despacho local por `switch` em `BR` (tabelas de salto não resolvidas estaticamente) e
  37.249 chamadas passam por `rt_dispatch`: correto, mas pesa no tamanho e no desempenho.
- As 830 funções descobertas por alvo de `BL` vão até o próximo início conhecido; uma delas pode ter fim impreciso.

## Testes

- O teste de função usa `-fno-vectorize` por padrão e oferece `--vectorize` para também testar NEON.
- O Unicorn mantém o monitor exclusivo entre execuções; o runner executa `CLREX` antes de cada caso para começar do
  mesmo estado que o harness. (Sem isso, 1 caso em ~60 mil falhava por vazamento de estado do Unicorn.)
- No Windows o Unicorn provoca uma exceção de acesso tratada internamente durante `mem_map`; o `faulthandler` do
  pytest foi desligado em `pytest.ini` para não reportá-la como fatal.
- O Zig ignora `-mgeneral-regs-only` e liga o UBSan em `-O0`; os scripts passam as opções necessárias.

## Ferramentas

- `RequiredSystemVersion` do meta da atualização vale 3694008467456 no XML, que não é uma palavra de versão de
  32 bits. Significado não confirmado; o relatório mostra o valor bruto.
- A versão exibida ("1.0.2"?) da atualização só pode ser confirmada com o `control.nacp`.
