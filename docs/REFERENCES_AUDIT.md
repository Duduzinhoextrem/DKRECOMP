# Auditoria de repositórios sugeridos

Antes de usar qualquer ferramenta de terceiros, o DKRecomp verifica procedência, licença, o que o código faz e se
ela ajuda de fato. Os repositórios foram clonados em `refs/untrusted/` (ignorada pelo Git) e **nada foi executado**.

## 2026-10-30

### inematds/rea-engenharia-reversa

| Item | Achado |
|---|---|
| O que é | Site/guia em PT/EN/ES sobre a ferramenta REA (HTML, imagens, legendas de vídeo). Não contém ferramenta própria |
| Autor | Conta de ~1,7 ano com **788 repositórios públicos**: padrão de publicação de conteúdo em massa |
| Histórico | 17 commits em 6 dias, um único autor; repositório de 3 semanas; licença MIT |
| Código | Um script `pratica/rodar-isolado.sh` que roda `npx -y rea-agents@6.4.0` (baixa e executa o pacote npm) e um workflow de GitHub Pages padrão. Nada malicioso no próprio repositório |
| Conteúdo | Material de divulgação (chamadas para INEMA.CLUB); uma seção "Sem travas" recomenda modelos de IA sem restrições (abliteration.ai, OrcaRouter) |
| Utilidade para o DKRecomp | Nenhuma direta. É uma explicação de outra ferramenta |

### morluto/rea (a ferramenta por trás do guia, pacote npm `rea-agents`)

| Item | Achado |
|---|---|
| O que é | Agente de engenharia reversa que orquestra Ghidra/IDA/Hopper e analisa apps JavaScript/Electron |
| Licença | MIT |
| Procedência do pacote | npm `rea-agents` 6.8.0, 34 versões, mantenedor `morluto`, com atestado de proveniência (compilado pelo GitHub Actions a partir do repositório) |
| Sinal de alerta | **411.667 estrelas e 103.007 forks em 6 meses**: muito acima de qualquer ferramenta de nicho. Estrelas infladas são uma tática conhecida para dar credibilidade a pacotes maliciosos. **Não verificado**: a API de stargazers exige login |
| Risco ao executar | `npx -y` baixa e executa o código com as permissões do usuário; o pacote tem 42 dependências diretas e abre terminais (`node-pty`, `cross-spawn`) |
| Nota | O TotK Recomp vendorizou uma *skill* de agente desse projeto (texto, não código executável) |
| Decisão | **Não executar nesta máquina.** Se for avaliado no futuro: só numa VM descartável, sem os arquivos do jogo nem credenciais |

### TomOstt/SwitchBlade

| Item | Achado |
|---|---|
| O que é | Projeto de estudo (1.275 linhas Python) para analisar o **firmware** do Switch (serviços do sistema) e procurar vulnerabilidades. Não é voltado a jogos |
| Autor / histórico | Conta pequena (8 repositórios, 4 seguidores), 26 commits em 11 dias (mar–abr/2026) |
| Licença | **Nenhuma**: todos os direitos reservados. Não podemos copiar código dele |
| Segurança | Sem execução de comandos, `eval`, downloads, rede externa ou ofuscação. A interface é uma API FastAPI local. Não parece malicioso |
| Qualidade | O loader de NSO **não descomprime LZ4** e, em segmentos comprimidos, recorta bytes do tamanho errado: não funciona com NSOs de jogos, que normalmente são comprimidos. O "decodificador" só classifica instruções em grupos grosseiros; o "decompilador" gera pseudocódigo em texto a partir do Capstone, sem semântica exata |
| Utilidade para o DKRecomp | Nenhuma. O DKRecomp já tem leitor NSO com LZ4 e verificação de hash, e decodificador exato testado contra o Unicorn |

### Tópico GitHub `game-reversing`

Lista aberta: qualquer repositório se marca com o tópico, então não indica confiabilidade. Em 2026-10-30: 19
repositórios, nenhum sobre Switch. Os mais estrelados são listas de links e patches de outros jogos/emuladores.
Um deles (`reagithu/REA-Github`) se apresenta como redistribuição do REA por uma conta desconhecida, padrão comum
de distribuição de malware: **evitar**.

## Conclusão

Nenhum dos itens sugeridos faz a engenharia reversa do jogo nem acrescenta algo que o DKRecomp ainda não tenha.
Para analisar o executável do jogo, as ferramentas estabelecidas são:

- Ghidra (NSA, Apache-2.0) com o Ghidra-Switch-Loader (usados pelo TotK Recomp);
- as ferramentas do próprio DKRecomp (`dkrecomp.identify`, decodificador e lifter).

Todas dependem da ExeFS extraída pelo usuário (`docs/REQUIRED_FILES.md`).
