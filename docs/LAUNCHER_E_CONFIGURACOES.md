# Launcher, configurações, resolução e FPS

Guia do launcher (`DKRecomp.bat`), das configurações lidas pelo jogo (`config.local.toml`) e do que foi medido sobre
resolução, taxa de quadros e vídeos. Registro de 2026-11-03.

## Como usar

1. Dê dois cliques em **`DKRecomp.bat`** na raiz do repositório. Ele abre o launcher (`tools/dkrecomp/launcher.py`,
   Tkinter, sem dependências novas).
2. Na aba **Início**:
   - **Preparar a partir do NSP…**: escolha o NSP do jogo base (v0). O launcher usa o `nstool.exe` e o `prod.keys` da
     pasta `SwitchExtract/` (ou pergunta onde estão), extrai o necessário, gera o C e compila. Etapas já feitas são
     puladas.
   - **Compilar**: só gera e compila de novo (depois de atualizar o código do DKRecomp).
   - **Jogar**: grava as configurações e abre `generated/dkctf_v0/dkctf.exe`.
3. As outras abas mudam as configurações. **Salvar** grava `config.local.toml`; **Jogar** também grava.

O estado na aba Início mostra se a ExeFS, a `romfs.bin` e o executável existem e se o código do DKRecomp é mais novo
que o executável (nesse caso, use **Compilar**).

## Preparar a partir do NSP

| Etapa | Ferramenta | Observação |
|---|---|---|
| Ler os metadados e reconhecer a versão | `dkrecomp.identify` (texto claro do NSP) | Recusa outro título; só a base `0100C1F0051B6000` v0 é suportada |
| Copiar o NCA de programa e o ticket para `user_files/_setup` | leitor PFS0 do próprio DKRecomp | O NSP é um contêiner simples; nada é decriptado. O `nstool` 1.9.2 não extrai um arquivo isolado de NSP no Windows |
| Extrair a ExeFS (`user_files/exefs`) | **seu** `nstool -k prod.keys --tik … --part0` | Conferida: byte a byte igual à ExeFS extraída antes |
| Extrair a RomFS (`user_files/romfs`, ~7 GB) | **seu** `nstool … --part1` | |
| Montar `user_files/romfs.bin` | `dkrecomp.romfs` | |
| Conferir a ExeFS, gerar o C e compilar | `dkrecomp.identify`, `dkrecomp.gen --gfx`, `ninja` | ~10 min na primeira compilação |

- O DKRecomp **não decripta nada nem abre o arquivo de chaves**: ele só passa o caminho do seu `prod.keys` para o seu
  `nstool` (ver `docs/LEGAL.md`). Nenhuma chave aparece em log.
- Espaço: preparar do zero precisa de ~21 GB livres (NCA temporário + RomFS + `romfs.bin`); o launcher confere antes.
  O NCA temporário é apagado no fim.
- O `nstool` às vezes falha com código de saída 0. Por isso cada etapa confere se o arquivo esperado foi criado.

## Configurações (`config.local.toml`)

O arquivo fica na raiz do repositório e é ignorado pelo Git. Quem lê é `runtime/src/settings.c`, no início do jogo.
Qualquer chave também pode vir de uma variável de ambiente `DKR_<CHAVE>` (por exemplo `DKR_RENDER_SCALE=2`), que tem
prioridade sobre o arquivo. Valores inválidos mantêm o padrão e aparecem no log.

| Chave | Valores | Padrão | Efeito |
|---|---|---|---|
| `display_mode` | `"windowed"`, `"borderless"` | `"windowed"` | Janela ou tela cheia sem bordas no monitor da janela. **Alt+Enter** ou **F11** alternam durante o jogo |
| `window_width`, `window_height` | 320–16384, ou 0 | 0 | Tamanho da área da janela; 0 = 75% da área útil do monitor, 16:9 |
| `render_scale` | 0.5–4.0 | 1.0 | Tamanho dos render targets do jogo (o original é 1920×1080). 2.0 = 3840×2160 |
| `fps_limit` | 0–1000 | 60 | Limite de FPS do host; 0 = sem limite |
| `vsync` | `"mailbox"`, `"on"`, `"off"` | `"mailbox"` | Modo de apresentação Vulkan: mailbox (sem tearing, baixa latência), FIFO ou imediato |
| `present_filter` | `"linear"`, `"nearest"` | `"linear"` | Filtro da imagem final ao escalar para a janela |
| `keep_aspect` | `true`, `false` | `true` | Barras pretas em vez de esticar quando a janela não é 16:9 |
| `show_stats` | `true`, `false` | `false` | FPS no título da janela e estatísticas `perf:` no log (o mesmo que `DKR_PERF=1`) |
| `mute` | `true`, `false` | `false` | Processa o áudio sem abrir o dispositivo do Windows (o mesmo que `DKR_AUDIO_NULL`) |

`DKR_CONFIG` aponta para outro arquivo.

## Resolução

- **Tamanho da janela / tela cheia**: a janela antes era fixa em 1280×720 e mostrava os 1920×1080 do jogo reduzidos.
  Agora ela segue a configuração, e a imagem é escalada com filtro linear e proporção mantida.
- **Resolução de renderização (`render_scale`)**: render targets 2D de pelo menos 128×64 são criados N vezes maiores
  na primeira vez que viram alvo de desenho; texturas do jogo (assets) não mudam. Todos os alvos de um draw usam a
  mesma escala. Viewport, scissor e cópias entre texturas são multiplicados; cópias entre escalas diferentes viram
  blit; uploads da CPU num alvo escalado passam por uma imagem intermediária. Do lado dos shaders, o tradutor do
  Ryujinx já escala `texelFetch` e `textureSize` e divide `gl_FragCoord` pelo array `render_scale` do support buffer;
  o replay preenche esse array por draw (`render_scale[0]` = alvo, `1 + i` = i-ésima textura do fragment shader em
  ordem de binding, depois as do vertex shader).
- **Medido**: escala 2× (3840×2160 internos) numa janela 1920×1080 mostrou o Play Mode correto a **60 fps**, com bordas
  visivelmente mais suaves que em 1×. As outras telas e a fase em si ainda não foram verificadas nessa escala.

## Taxa de quadros

- **Medido**: com `fps_limit = 0` e VSync desligado, o título rodou a **80–220 fps** (média ~80 com cena 3D).
- **O jogo não acelera acima de 60**: a transição título → Game Select levou o mesmo tempo real (~1,3 s, terminando em
  10,64 s e 10,66 s) a 60 fps e destravado, embora a execução destravada tenha desenhado 608 quadros contra 560. As
  animações usam tempo real, não contagem de quadros. Isso foi verificado **só nos menus**; física e gameplay na fase
  ainda precisam ser conferidas antes de recomendar mais de 60.
- **VSync FIFO + limitador**: os dois esperando juntos deram ~53 fps num limite de 60. Agora o limitador do host só
  atua abaixo da taxa do monitor quando o VSync é FIFO. O padrão continua **mailbox**.
- Para passar de ~80–100 fps de forma estável, o próximo gargalo é a espera síncrona pela GPU a cada envio
  (`dkr_vk_batch_end`) e as cópias de vértices (~20 MB por frame). Ver `docs/KNOWN_ISSUES.md`.

## FPS dos vídeos (pesquisa)

Os vídeos do jogo são H.264 a **29,97 fps** (`intro.mp4`: 1280×720). O decodificador entrega exatamente essa taxa
(30 quadros/s, medido), igual ao console. Mais quadros exigem **interpolação**, que cria quadros intermediários:

| Opção | Licença | Avaliação |
|---|---|---|
| [RIFE via rife-ncnn-vulkan](https://github.com/nihui/rife-ncnn-vulkan) | MIT | A melhor qualidade para vídeo pronto. Viável como etapa **offline** do launcher: gerar uma versão 60 fps de cada MP4 uma vez (minutos por vídeo na GPU) e o decodificador entregar 2 quadros por pacote, com tempos intermediários. Precisa testar se o player do jogo aceita mais quadros que o ritmo do arquivo |
| [AMD FSR 3 Frame Interpolation / Optical Flow](https://gpuopen.com/fsr3/) | MIT | Feito para jogos: depende de vetores de movimento do motor e o caminho Vulkan ainda era limitado. Ruim para vídeo pré-renderizado |
| Frame Rate Converter DSP do Windows | sistema | Só repete ou descarta quadros, não interpola: nenhum ganho visual |
| NVIDIA Optical Flow FRUC | proprietária | Fora da política de licenças do projeto |

Recomendação: RIFE offline como opção do launcher ("vídeos a 60 fps"), sem alterar os arquivos originais do jogo.
Ainda não foi implementado.

## Arquivos

| Arquivo | Papel |
|---|---|
| `DKRecomp.bat` | Abre o launcher com o Python do `.venv` |
| `tools/dkrecomp/launcher.py` | Interface, configurações, preparação a partir do NSP, compilar e jogar |
| `runtime/include/dkr/settings.h`, `runtime/src/settings.c` | Leitura de `config.local.toml` e `DKR_*` |
| `runtime/gfx/vk.c` | Janela/tela cheia, Alt+Enter, VSync, filtro e proporção, FPS no título |
| `runtime/gfx/nvn_replay.c`, `vk_draw.c` | Escala de resolução, blit entre escalas |
| `runtime/gfx/nvn_obj.c` | Limite de FPS |
| `tests/python/test_launcher.py` | Configurações, plano de preparação, cópia do NSP e abertura da janela |
