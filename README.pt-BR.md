# DKRecomp

English: [README.md](README.md)

O DKRecomp transforma a versão de Nintendo Switch de *Donkey Kong Country: Tropical Freeze* num programa nativo para
Windows x86-64. É um recompilador estático: o código ARM64 do jogo é traduzido para C uma única vez, no seu próprio
computador, e compilado com Clang. Nenhum emulador roda enquanto você joga.

Este repositório não contém arquivos do jogo. Você precisa da sua própria cópia, e é você quem faz a conversão.
O DKRecomp não decripta nada, não baixa conteúdo do jogo e nunca abre as suas chaves. As regras estão em
[docs/LEGAL.md](docs/LEGAL.md).

## Estado

O jogo inicia, mostra os menus e os vídeos, e as fases são jogáveis, inclusive as fases especiais. Na máquina do
mantenedor (RTX 5060 Ti) ele roda a 60 fps, com resolução interna opcional de até 4x. Ainda há bugs: veja
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) e o histórico medido em [docs/PROGRESS.md](docs/PROGRESS.md).

Só o jogo base, versão 1.0.0 (título `0100C1F0051B6000`, v0), é suportado.

## Requisitos

- Windows 10 ou 11 de 64 bits, num processador com FMA (Intel Haswell, AMD Zen ou mais novo).
- Placa de vídeo com driver que suporte Vulkan 1.3. Algumas cenas também usam as extensões
  `VK_EXT_depth_clip_control` e `VK_EXT_transform_feedback`.
- Uns 20 GB livres durante a preparação. A RomFS extraída e a imagem da RomFS ocupam cerca de 7 GB cada.
- Python 3.13, Git e o .NET 10 SDK.
- Da sua cópia do jogo: o NSP do jogo base, o seu `nstool.exe` e o seu `prod.keys`. Quem decripta é o `nstool`, na sua
  máquina; o DKRecomp só repassa a ele os caminhos que você escolher.

## Preparação

```bash
git clone <este repositório> DKRECOMP
cd DKRECOMP
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

O tradutor de shaders é compilado a partir de uma revisão fixa do projeto TotK Recomp, que traz o compilador de
shaders do Ryujinx (licença MIT):

```bash
git clone https://github.com/valentderah/totk-pc-recomp refs/totk-pc-recomp
git -C refs/totk-pc-recomp checkout c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784
```

Depois, dê dois cliques em `DKRecomp.bat`:

1. **Preparar a partir do NSP…** pede o NSP, o `nstool.exe` e o `prod.keys`. O launcher extrai o código e os dados,
   gera o código C e compila. A primeira compilação leva uns 10 minutos. Na primeira vez ele também baixa o SDL3
   (controles) e os headers do Vulkan das fontes oficiais.
2. **Jogar** abre o jogo.

As outras abas mudam as configurações, gravadas em `config.local.toml`. As opções estão em
[docs/LAUNCHER_E_CONFIGURACOES.md](docs/LAUNCHER_E_CONFIGURACOES.md).

Para não ver a janela de console, `.venv/Scripts/python tools/build_launcher_exe.py` cria o `DKRecomp.exe`, que abre o
mesmo launcher. Para jogar em outro computador seu sem instalar Python nele,
`.venv/Scripts/python tools/make_portable.py` copia o jogo compilado para `DKRecomp-Portatil/`. Essa pasta contém dados
da sua cópia do jogo, então não a compartilhe.

## Controles

Controles de Xbox, DualShock 4 e DualSense funcionam pelo SDL3, com XInput como alternativa. No teclado: WASD ou setas
para andar, Espaço ou K para pular, J ou Q para atacar e rolar, L ou E para confirmar, Enter ou Esc para pausar,
Alt+Enter ou F11 para tela cheia.

## Como funciona

- `tools/dkrecomp/` lê arquivos NSP, NSO e NPDM, decodifica o ARM64, converte cada função para C e liga os módulos.
- `runtime/src/` implementa as partes do Horizon OS que o jogo chama: threads e sincronização, IPC e os serviços de
  entrada, áudio, sistema de arquivos e saves. Os vídeos são decodificados pelo Media Foundation do Windows.
- `runtime/gfx/` registra os comandos NVN do jogo e os reproduz em Vulkan. Os shaders Maxwell são traduzidos para
  SPIR-V pelo compilador de shaders do Ryujinx, que roda num processo separado (`tools/shader_translator/`).
- `tests/` confere o recompilador contra o emulador de CPU Unicorn: o mesmo código ARM64 roda nos dois e precisa dar o
  mesmo resultado.

Mais detalhes em [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Desenvolvimento

```bash
.venv/Scripts/python -m pytest
```

Os testes nativos e de Vulkan usam CMake e estão em [docs/TESTING.md](docs/TESTING.md). Antes de cada push, rode
`.venv/Scripts/python tools/check_content.py`: ele falha se algum arquivo do jogo, ou derivado dele, estiver no Git.

## Créditos e licença

Projetos de referência: TotK Recomp, LAS Recomp, SwitchRecomp e N64Recomp. O código vindo de outros projetos, com a
respectiva licença, está listado em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

A licença do código do DKRecomp ainda não foi escolhida. Nenhuma licença deste repositório cobre o jogo.

O DKRecomp é um projeto independente, sem afiliação com a Nintendo ou a Retro Studios. *Donkey Kong Country*,
*Tropical Freeze* e *Nintendo Switch* são marcas da Nintendo.
