# Problemas conhecidos

## Bloqueios

| # | Problema | Impacto | Saída |
|---|---|---|---|
| B1 | Extração dos arquivos proprietários | Resolvido para o alvo **base 1.0.0**: ExeFS e RomFS disponíveis localmente | Outras versões exigem seus próprios arquivos extraídos; DKRecomp não decripta |
| B2 | O Visual Studio 2026 desta máquina não tem o Windows SDK (falta `kernel32.lib` e os headers da UCRT) | MSVC não compila nem liga; a compatibilidade MSVC do C gerado não foi verificada | Instalar o componente "Windows 11 SDK" pelo Visual Studio Installer (mudança no sistema: **requer autorização do usuário**). Enquanto isso, Clang do Zig |

## Limitações do recompilador

- O decodificador cobre 100% do `main` e do `subsdk0`. **No `sdk` faltam** as extensões de criptografia (AES,
  SHA-1, SHA-256, PMULL), CRC32, `CNTPCT_EL0`, `CTR_EL0` e formas saturadas (`sqadd`, `sqrshrn`): 67 funções.
- Os auxiliares SIMD (`simd.h`) usam laços genéricos por elemento: corretos, mas lentos. Especializar por tamanho de
  elemento é trabalho de desempenho para depois do primeiro boot.
- **Só FPCR = 0 é modelado**: arredondamento ao mais próximo, sem flush-to-zero e sem modo default-NaN. Se o jogo
  escrever outro valor em FPCR, o resultado de FP pode divergir. Ainda não há aviso em tempo de execução para isso.
  As flags acumuladas de exceção do FPSR não são mantidas.
- O C gerado exige **CPU com FMA** (`-mfma`, Haswell/Zen ou mais novo), porque o FMADD precisa de multiplicação-soma
  fundida exata. É o mesmo requisito do TotK Recomp e do LAS Recomp.
- Não suportados também: `DC ZVA` (o `DCZID_EL0` informa proibido), CRC32, LSE (ARMv8.1, não esperado no Switch),
  pares exclusivos (`LDXP/STXP`), `CAS`, instruções de sistema além de NZCV/TPIDR/TPIDRRO/FPCR/FPSR/CNTVCT/CNTFRQ.
- Sobreposições de registradores CONSTRAINED UNPREDICTABLE em loads/stores (ex.: `ldp x0, x0`) são rejeitadas de
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

- Parada atual: `audren:u:OpenAudioRenderer` (0). GetWorkBufferSize (1) aceita REV1–4 e execução DSP/automática;
  REV5+, CPU/manual, IAudioRenderer, RequestUpdate, evento periódico e saída de som ainda faltam.
  O SDK aceitou 0x9e000 de work buffer; isso não comprova processamento de áudio.

- Bloqueio atual com gráficos do host: **`audren:u` comando 0** (OpenAudioRenderer), depois da inicialização
  dos recursos NVN sem renderização (3102 SVCs na execução mais recente). O objeto de áudio ainda não existe.
  Os rings HID ainda não publicam entrada; controles ficam desconectados.
  A lista de vibração registra os dispositivos ativados, mas rumble do host e envio de valores ainda faltam.
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
- O filesystem implementa a RomFS somente leitura e a montagem do save de conta. Operações de arquivos do save,
  SD card e outros tipos de IFileSystem/IFile ainda faltam. A raiz de save suporta caminhos locais de drive;
  UNC e reparse points são recusados. A imagem tem que ser regenerada se os arquivos extraídos mudarem; ela não é o NCA original nem contém
  suas camadas de integridade. A abertura verifica tamanho e header, sem auditoria completa de todos os metadados.

## Módulo gerado

- O endereço base do `main` (`0x800000000`) é **provisório** e fica embutido no C gerado; mudar o layout exige
  regerar e recompilar (~5 minutos).
- 5.725 funções usam o despacho local por `switch` em `BR` (tabelas de salto não resolvidas estaticamente) e
  37.249 chamadas passam por `rt_dispatch`: correto, mas pesa no tamanho e no desempenho.
- As 830 funções descobertas por alvo de `BL` vão até o próximo início conhecido; uma delas pode ter fim impreciso.
- O boot executa código gerado dos três módulos e inicia uma segunda thread. Isso e os testes diferenciais não
  comprovam todo o comportamento do jogo; gráficos, áudio e gameplay continuam pendentes.

## Gráficos

- **Nada é desenhado.** Os objetos NVN existem e os comandos são gravados e aceitos no submit, mas não há replay
  Vulkan, tradutor de shaders nem upload de texturas. Toda submissão conta como concluída na hora (fences e
  syncs sinalizam imediatamente). O present só cadencia a 60 Hz/intervalo; a janela não mostra o jogo.
- Os registros de comando guardam só os registradores de argumento. Ponteiros (arrays de viewports, estados,
  dados de uniform) não têm o conteúdo capturado na gravação; o replay terá que fazer isso por comando.
- Memória de comando e de controle do jogo nunca é consumida: os getters relatam tudo livre e o callback de falta
  de memória nunca roda.
- Pools físicos/virtuais (MapVirtual) param com relatório; `QueueGetError` não escreve o info (layout do 53.105
  desconhecido). `nvnTextureBuilderGetZCullStorageSize`, `GetSparseTileLayout` e `TextureViewCompare` ainda param.
- O tamanho automático de bloco das texturas segue a regra medida no NVN 55; os 40 casos de teste também vêm do
  NVN 55. Conferir contra texturas empacotadas reais do jogo.
- A análise de pureza é conservativa e depende de offsets do `sdk` 1.0.0 (`nvnmap.py`); outra versão do SDK
  falha na verificação de ordenação dos nomes.
- A lista MIT de nomes NVN não representa cobertura de implementação: procs sem implementação encerram a
  execução quando chamados; nomes desconhecidos devolvem NULL.
- A janela atual é Win32 e usa uma fila com graphics+present, Vulkan 1.1 e swapchain RGBA/BGRA UNORM com suporte
  a transferência/readback. Outros formatos e plataformas ainda não foram implementados.
- A camada de validação é opcional no build, mas foi usada localmente com sincronização ativa. A ferramenta
  nightly é identificada pelo commit/digest em `toolchain/vulkan-validation/source.txt` e não é distribuída.
- O loader local registra um manifesto antigo ausente do overlay Epic e avisos de nomes Galaxy. Nenhuma
  configuração do sistema foi alterada; esses diagnósticos gerais ficam no log e não contam como erros VUID.

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
