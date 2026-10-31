# Pesquisa para chegar a uma versão jogável

Pesquisa de 2026-10-31. O critério continua sendo abrir o título e jogar uma fase com imagem e controle reais.
Uma janela vazia, contadores de apresentação ou testes sintéticos não satisfazem esse critério.

## Implementações encontradas

| Fonte | Evidência publicada e utilidade para o DKRecomp |
|---|---|
| [nxnative, Tropical Freeze](https://github.com/seedds/nxnative/blob/5474c6f34789a41770868564fbf4e9e32fc57584/dm/games/0100C1F0051B6000/README.md) | Relata título, seleção de jogo/controle, introdução e primeira fase funcionando no Mac, com SDK 4.4.0. O teste publicado usa atualização 1.0.2; nosso alvo é 1.0.0. Isso fornece uma rota técnica, sem comprovar o funcionamento local. |
| [nxnative, renderizador de áudio](https://github.com/seedds/nxnative/blob/5474c6f34789a41770868564fbf4e9e32fc57584/dm/host/hle_audren.c) | Usa o servidor CPU/manual e o processador de comandos já presentes no SDK. O host dirige uma thread por buffers de áudio e extrai PCM do DeviceSink. Evita reimplementar vozes, ADPCM, mixagem e efeitos. MIT, origem preservada nos arquivos adaptados. |
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp/tree/c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784) | Referência principal do back-end Vulkan no Windows e do tradutor Maxwell → SPIR-V baseado no Ryujinx. Cpu, objetos NVN e versão do driver diferem; exigir medição no nosso SDK. Checkout local somente leitura. |
| [libnx, audren](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/audren.h), [Audio services](https://switchbrew.org/wiki/Audio_services) | Referências públicas para revisão, parâmetros internos, memória das vozes, comandos IPC e eventos. Úteis para conferir o protocolo e manter o modo legado; o caminho CPU utiliza o próprio parser do SDK. |
| [Microsoft, saída por evento WinMM](https://learn.microsoft.com/en-us/windows/win32/multimedia/using-an-callback-to-process-driver-messages) | Contrato de buffers preparados, flags de conclusão e evento. Usado na saída Windows inicial; a API recomendada pela Microsoft para um back-end futuro é WASAPI. |

## Decisões de implementação

1. **Áudio:** testar diretamente o servidor CPU/manual do SDK 4.4, preservando a necessidade DSP de 0x9e000
   bytes que o jogo solicita. O armazenamento adicional do back-end fica por conta do host. O evento do jogo
   deve continuar sendo um evento de kernel: a referência encontrou bloqueio na troca de fase com evento local.
2. **Gráficos:** continuar na fronteira NVN do host, com execução Vulkan e tradução Maxwell. Atender ioctls de
   `nvdrv` sem interpretar comandos Maxwell não resolveria a renderização. O .NET 10 já está instalado nesta
   máquina; testar build managed do tradutor evita depender do Windows SDK ausente para NativeAOT.
3. **Comandos e shaders:** capturar os dados apontados durante a gravação; depois executar os comandos e
   traduzir shaders reais. A referência do Tropical Freeze descreve shaders em RFRM/SNVN e particularidades
   de uploads CPU_UNCACHED e transform feedback. Confirmar offsets no alvo 1.0.0 antes de aplicá-los.
4. **Jogabilidade:** publicar estados HID do teclado/controle, completar as operações de save solicitadas,
   conferir título, carregamento da fase e movimento/salto no executável. Áudio, estabilidade e desempenho
   também precisam de validação real. A pesquisa não elimina esses trabalhos.

## Evidência local desta tentativa

O primeiro boot com o caminho CPU/manual abriu o renderer e a saída WinMM em 48 kHz estéreo, quatro buffers
de 240 amostras. O orçamento CPU medido foi **0xa0000**, contra **0x9e000** exposto ao jogo. A execução chegou
a `SQRSHRN` e depois a `SQADD` no processador DSP recompilado, antes ausentes no lifter. Não houve imagem nem
som confirmado nessa tentativa.

As novas formas vetoriais de narrowing saturado e soma saturada foram comparadas com o Unicorn:
**924 codificações, 14.784 casos, zero diferenças**, incluindo FPSR.QC. Multiplicações por elemento passaram
em **6.000 casos**, dos quais 2.376 cobrem as novas formas longas. Os próximos boots devem mostrar se ainda
há bloqueios antes do loop de apresentação. O resultado atual deve ser consultado em `PROGRESS.md`.
