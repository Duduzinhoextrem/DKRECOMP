# DKRecomp

Ferramenta de **recompilação estática** que, a longo prazo, deve converter a versão Nintendo Switch de
*Donkey Kong Country: Tropical Freeze* num programa nativo para Windows x86-64, sem emulador durante a execução.

> **Ainda não há gráficos nem gameplay.** O executável nativo já inicializa o SDK, chega ao `nnMain`, executa
> uma segunda thread e lê a RomFS. O boot para com relatório no primeiro recurso pendente.
> Veja o estado medido em [docs/PROGRESS.md](docs/PROGRESS.md).

> **Nenhum arquivo do jogo está incluído**, e o DKRecomp não decripta nem baixa nada. Você precisa da sua própria
> cópia legítima. Veja [docs/LEGAL.md](docs/LEGAL.md) e [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md).

## O que já funciona

- `dkrecomp.identify`: lê NSPs (contêiner PFS0, ticket, metadados), confere o SHA-256 de cada conteúdo e reconhece
  a versão; lê ExeFS extraídas (NSO, NPDM, imports, SVCs, middleware).
- Decodificador e recompilador ARM64 → C (inteiros, desvios, memória, atômicos, FP escalar e SIMD vetorial),
  cobrindo as instruções das funções com símbolo de `main` e `subsdk0` da versão base 1.0.0.
- Ligação estática dos módulos, loader e boot do código recompilado, threads do Windows, sincronização, IPC e
  serviços iniciais do Horizon. Imagem RomFS gerada por streaming a partir dos arquivos já extraídos do usuário.
- Testes diferenciais: o mesmo código ARM64 roda no Unicorn e recompilado, e precisa dar o mesmo resultado
  (dezenas de milhares de instruções aleatórias e programas C inteiros em O0/O1/O2/Os).

## Jogar

Dê dois cliques em **`DKRecomp.bat`**. O launcher prepara o jogo a partir do **seu** NSP (com o seu `nstool` e as suas
chaves), compila e abre o jogo, com opções de janela/tela cheia, resolução de renderização até 8K, limite de FPS,
VSync e áudio. Guia: [docs/LAUNCHER_E_CONFIGURACOES.md](docs/LAUNCHER_E_CONFIGURACOES.md).

Prefere um executável? `.venv/Scripts/python tools/build_launcher_exe.py` cria **`DKRecomp.exe`** na raiz: ele abre o
mesmo launcher, sem janela de console. Deixe-o nessa pasta (ou use um atalho para ele).

Para jogar em outro computador seu sem instalar nada além do driver de vídeo e do .NET 10 Runtime:
`.venv/Scripts/python tools/make_portable.py` copia o jogo já compilado para `DKRecomp-Portatil/` (~7 GB, com
`DKRecomp.exe` e um `LEIA-ME.txt`). Copie a pasta inteira; o `DKRecomp.exe` dela abre o jogo direto.

## Começar (desenvolvimento)

Requisitos: Windows 10/11 x86-64, Python 3.13, Git.

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

```bash
.venv/Scripts/python -m pytest
```

Identificar seus arquivos (relatório em `reports/`, ignorado pelo Git):

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify "game original meu" --hash
```

Mais comandos em [docs/TESTING.md](docs/TESTING.md).

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/FEASIBILITY.md](docs/FEASIBILITY.md) | Viabilidade: fatos, hipóteses, incógnitas, comparação com o TotK Recomp |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modelo de execução e organização do código |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Marcos (MVP 1–6) e critérios de aceitação |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Progresso real, com evidências |
| [docs/LAUNCHER_E_CONFIGURACOES.md](docs/LAUNCHER_E_CONFIGURACOES.md) | Launcher, configurações, resolução, FPS e vídeos |
| [docs/TESTING.md](docs/TESTING.md) | Como testar e o que cada teste prova |
| [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) | Limitações e bloqueios |
| [docs/LEGAL.md](docs/LEGAL.md) | Regras sobre arquivos proprietários |
| [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md) | Arquivos do jogo necessários em cada fase |
| [docs/REFERENCES_AUDIT.md](docs/REFERENCES_AUDIT.md) | Auditoria de confiabilidade dos repositórios sugeridos |

## Créditos e licença

Projetos de referência: TotK Recomp, LAS Recomp, SwitchRecomp, N64Recomp. A lista MIT de nomes públicos NVN foi
incorporada; atribuição, licenças e ferramentas em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Licença do código do DKRecomp: **a definir pelo mantenedor** (MIT é a opção compatível com os projetos de
referência). Nenhuma licença do projeto cobre o jogo.

Projeto independente, sem afiliação com a Nintendo ou a Retro Studios. *Donkey Kong Country*, *Tropical Freeze* e
*Nintendo Switch* são marcas da Nintendo.
