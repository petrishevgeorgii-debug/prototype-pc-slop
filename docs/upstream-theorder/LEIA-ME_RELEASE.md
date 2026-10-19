# The Order: 1886 — Port nativo para PC (Vulkan): notas da versão 0.6.1

Port em desenvolvimento de **The Order: 1886 (PS4, CUSA00035 versão 01.02)** para Windows. A
documentação completa está em [README.pt-BR.md](README.pt-BR.md) e o registro técnico em
[docs/PROGRESS.md](docs/PROGRESS.md). Nenhum arquivo do jogo está incluído: use o dump da sua
própria cópia.

## Como jogar (pacote Windows)

1. Baixe `theorder1886-windows.zip` e extraia a pasta em qualquer lugar (não rode de dentro do zip).
2. Abra **`The Order 1886.exe`**. Se o Windows avisar ("O Windows protegeu o computador"), clique
   em "Mais informações" e "Executar assim mesmo": o programa não é assinado digitalmente.
3. Na página **Jogo**, escolha a pasta do dump (a que tem `eboot.bin`, `sce_module` e `sce_sys`).
4. Clique em **JOGAR**. A primeira execução demora mais (preparação do jogo e dos shaders).

`Play The Order 1886.exe` inicia o jogo direto com as últimas configurações (útil na Steam). Saves,
cache de shaders e logs ficam na pasta `user\` ao lado do launcher.

Para rodar a partir do código-fonte: `bash build.sh` e `python run.py`.

## Situação (18/10/2026, RTX 5060 Laptop, Windows 11)

Novidades da 0.6.1:

- **Núcleo de vídeo atualizado (shadPS4 0.20.1)** e **divisão de envios longos de comandos**
  (evita estouro de memória do driver em cenas pesadas como o Capítulo IX, #11).
- **Placas sem `fragmentShaderBarycentric` (GTX 10xx, Intel UHD/Iris, etc.)**: shaders que usam
  interpolação por vértice agora ganham um geometry shader automático de fallback (#1, #10, #15).
- **DLCs de trajes e armas (`addcont`)**: carregados automaticamente a partir da pasta do jogo (#16).
- **Patch de 60 FPS na versão europeia CUSA00076 01.00**: locais do limitador encontrados
  automaticamente (#12).
- **Tradução Vulkan e cache de texturas**:
  - Samplers ligados a texturas inteiras (`Sint`/`Uint`) têm filtros lineares/anisotrópicos
    convertidos para `NEAREST` automaticamente (`VUID-vkCmdDispatchIndirect-magFilter-04553`),
    com associação serializada no cache de pipelines (`ShaderMetaVersion = 11`).
  - Views inteiras `1920x800` reutilizam diretamente os render targets `1920x1080` já em cache sem
    re-upload ou cópia intermediária (`THEORDER_PARTIAL_HEIGHT_VIEWS=0` desativa).
  - Verificação de alterações no cache de texturas (`MaybeDirtyHash`) agora lê apenas o mip 0 em
    vez de todos os níveis de mipmap.
  - Fast path em `ImageResource::NumBindings` e proteção contra shaders vazios (`s_endpgm`).
- **Estabilidade na inicialização (TLS / MT19937)**: corrigida condição de corrida em `sub_89fa60`
  onde múltiplas worker threads recriavam chaves TLS globais e causavam crash no boot (`0xc00ed7`).
- **Endereços de shaders de 64 bits**: loads, stores e atomics MUBUF preservam o par de
  registradores de endereço e o carry dos offsets. A próxima execução recompila os shaders.
- **Testes Vulkan no Windows**: 42 alvos C/C++ (incluindo `shader-resource-test`, resolução, TAA e
  câmera) e 266 testes Python validados na suíte integrada (`tools/run_tests.sh`).
- **Presets de mods preservados** ao atualizar a cópia jogável em `dist`.
- Leitura de V# inline via BDA disponível para testes com `THEORDER_DIRECT_MEMORY_ACCESS=1`;
  permanece experimental e desligada por padrão.

Novidades da 0.6.0:

- **Mirar não borra mais a tela inteira**: o desfoque de profundidade da mira recebia distâncias
  de foco perto de zero, então tudo ficava fora de foco. O jogo calcula o foco lendo, na CPU, um
  buffer que a GPU escreve a cada frame com a profundidade da cena; esse buffer nunca era copiado
  de volta. Agora o port detecta sozinho os buffers que a CPU lê e os copia a cada frame, sem
  esperar a GPU. O alvo sob a mira fica nítido e o fundo desfoca, como no PS4. O mesmo buffer
  ajusta as cascatas de sombra.
- **Sombras que sumiam voltam**: a conversão dos mapas de sombra lê o HTILE (metadado da
  profundidade) para pular blocos vazios. Ninguém atualizava o HTILE, então blocos com objetos
  pareciam vazios e as sombras deles desapareciam.
- **Placas NVIDIA e Intel**: atomic min/max de float em shaders executava as duas operações em vez
  de uma (reduções de profundidade erradas). Só os shaders afetados são retraduzidos.
- Texturas carregadas por cópias da GPU deixam de ficar com o conteúdo antigo.
- **Filtragem de texturas** (Gráficos > Filtragem de texturas): 2x a 16x, pisos e paredes vistos
  de lado mais nítidos.
- Testes: `tools/run_tests.sh` roda os 35 testes C/C++ e a suíte Python; arquivos sem uso
  removidos (scripts antigos e os `.bat` com atalhos de nível que o jogo ignora).

Novidades da 0.5.0:

- **Interface em 16:9, 16:10 e 21:9**: HUD e legendas mantêm o tamanho e a posição originais,
  centralizados na imagem mais alta (antes cresciam ~7%, iam até as bordas e o título de capítulo
  passava da borda direita). Telas cheias da interface (menu de pausa) ocupam a faixa central
  2.40:1, como no jogo original, onde fora dela havia barras pretas.
- **Campo de visão (FOV)** no menu do jogo (Insert / R3+L2 > Display): 50-200%, aplicado na hora;
  acima de 100% mostra mais cenário em todos os lados.
- **Câmera pelo mouse**: o mouse gira a câmera; botão esquerdo atira (R2), direito mira (L2), meio
  R3, laterais L1/R1. F7 ou o menu desligam; sensibilidade no menu.
- **Bem menos objetos piscando**: um erro na cópia da chave dos pipelines fazia o jogo não achar
  pipelines já prontos e montá-los de novo (o objeto sumia de novo a cada vez). Corrigido, e os
  pipelines usados nas últimas sessões são restaurados em segundo plano ao abrir o jogo. Numa
  sessão de 150 s no telhado: de ~400-500 restaurações durante o jogo para ~40.
- A iluminação de partículas e fumaça não some mais enquanto uma variante nova do shader compila.
- Quando o patch de 60 FPS não é encontrado na sua versão do jogo, o `last_run.log` lista os
  locais candidatos para o patch poder ser adaptado.

Novidades da 0.4.1:

- **Bem menos objetos piscando**: enquanto um efeito ou objeto novo compila em segundo plano ele
  some por alguns quadros. Agora o que já está no cache volta na frente da fila e mais threads
  compilam ao mesmo tempo: na primeira sessão depois de uma atualização, ~3x menos piscadas (na
  entrada de uma cena, ~10x); nas sessões seguintes, ~40% menos.
- Launcher (correções vindas do launcher do God of War III PC):
  - "Fechar o launcher quando o jogo abrir" agora mantém o `last_run.log` completo;
  - em notebooks com GPU integrada AMD + NVIDIA, a camada Vulkan da AMD que escondia a NVIDIA
    (tela preta) é desligada;
  - quando o jogo fecha por falta de memória, por erro de memória do driver de vídeo ou por dump
    incompleto, o log mostra o que fazer;
  - dumps com `eboot.bin` já desempacotado (ELF) são aceitos;
  - backups de saves feitos no mesmo segundo não apagam o mais novo.

Novidades da 0.4.0:

- **Fim da queda para ~20 FPS em cenas pesadas** (ex.: o prédio depois do telhado): um aviso
  interno da GPU era gravado milhares de vezes por segundo no log, e o Windows grava cada
  caractere numa chamada separada. Agora cada linha é uma escrita só e esse aviso sai uma vez por
  shader. No telhado, modo 60 FPS: de ~31 para ~38 FPS; o `last_run.log` deixa de crescer ~15 MB
  por minuto.
- **Manchas pretas (hexágonos) no rosto em cutscenes e ao girar a câmera**: pixels NaN que o PS4
  descarta passavam adiante e o desfoque (depth of field) os espalhava. O tradutor de shaders
  agora segue as regras de NaN do hardware (correções do shadPS4 #5257 e #5346).
- **Atenção:** o cache de shaders é refeito uma vez; a primeira sessão depois de atualizar tem
  mais travadinhas enquanto ele se reconstrói.
- Erros do Vulkan agora dizem qual chamada falhou (para a issue #11).

Novidades da 0.3.9:

- **Fim das travadas de ~150 ms ao aparecer um objeto ou efeito novo**: o port às vezes compilava
  na hora uma pipeline que achava já ter salva. Agora ele só restaura o que está salvo e, se não
  houver, monta em segundo plano. No prólogo, o pior quadro caiu para ~100–120 ms (antes até
  500 ms); no telhado, nenhuma dessas compilações.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.8:

- **Versão europeia (CUSA00785) passa da abertura** (#7, #9): o jogo parava logo no início com
  "nonzero AppContent service label unsupported". Ainda não testada além desse ponto.
- **Intel (Arc, HD 630): personagens como silhuetas pretas** (#1, #10): o port não usa mais a
  extensão VK_EXT_vertex_input_dynamic_state em placas Intel (a mesma correção do port de God of
  War III). `THEORDER_VERTEX_INPUT_DYNAMIC_STATE=0/1` força em qualquer placa.
- **Todos os idiomas do PS4 no launcher** (#4, #5): a lista tinha 7; agora tem os 30 (chinês,
  coreano, turco, polonês, português do Brasil...). O jogo usa os que a versão dele tiver.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.7:

- **Sem as travadas de ~180 ms de efeitos de compute**: shaders de compute refeitos a cada quadro
  (como a iluminação das partículas) agora são sempre montados em segundo plano; o efeito pode
  faltar por um quadro na primeira vez, em vez de o jogo congelar. No prólogo, essas travadas
  sumiram (eram 1–3 por sessão).
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.6:

- **Início de cena sem a pausa longa**: ao entrar numa cena, o port restaurava ~100 pipelines
  salvas, uma após a outra, travando um quadro por 0,3 a 1,4 segundo. Agora, passados 8 ms
  dessas restaurações num quadro, as demais são feitas em paralelo em segundo plano. No prólogo, o
  pior quadro do início de cena caiu de 300+ ms para ~67 ms; ao carregar o telhado de Londres, de
  833 ms para 300–400 ms. Nos quadros logo depois, a cena aparece completa (as restaurações
  acontecem durante o carregamento).
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.5:

- **Crash corrigido em sessões longas** ("code is too big"): cada variação de shader guardava a
  própria cópia de um pequeno trecho de código gerado (o que lê as tabelas de recursos) num
  buffer de 32 MB que nunca é liberado. Em 80 segundos do prólogo eram ~3.000 cópias (26 MB)
  idênticas a ~200 originais; numa sessão mais longa, ou com um cache de shaders maior, o buffer
  enchia e o jogo fechava. Agora as cópias idênticas são compartilhadas (0,4 MB).
- **Menos travadas de ~170 ms**: algumas pipelines eram compiladas na hora, travando o jogo,
  porque o port as confundia com pipelines já salvas de sessões anteriores. Agora elas são
  montadas em segundo plano (o objeto pode aparecer um instante depois). No prólogo, essas
  travadas caíram de 4–8 para 2–4 por sessão.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.4:

- **Modo de 60 FPS mais rápido em cenas pesadas**: no telhado de Londres (~1.580 draws por
  quadro) o modo experimental de 60 FPS subiu de ~33 para ~39 FPS em média. Um quinto do tempo da
  thread que grava os draws ia para um lock consultado a cada recurso de cada draw (a checagem de
  que o endereço está mapeado para a GPU); agora cada thread consulta uma cópia própria, renovada
  só quando o jogo mapeia ou desmapeia memória. Nos 30 FPS do jogo, o telhado agora fica em 30
  (variava entre 28 e 30).
- O `theorder.ini` não guarda mais as chaves do port de God of War III (multiplicadores de orbes,
  trapaças...) que versões antigas escreviam.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.3:

- **Outras versões do jogo não fecham mais na abertura** (issue #3): o port escrevia ganchos de
  diagnóstico e patches nos endereços do eboot 01.02 em qualquer eboot; num eboot 01.00 isso
  corrompia o código do jogo, que fechava logo ao iniciar. Os ganchos agora só entram quando cada
  ponto tem a instrução esperada, e uma linha de patch sem assinatura só vale na 01.02. Os patches
  embutidos (60 FPS e proporção de tela) ganharam assinaturas e podem valer em outras versões com
  o mesmo código. A 01.02 continua sendo a única versão testada.
- **Texturas pretas ou faltando em algumas GPUs** (issue #1, Intel Arc): os workers que preparam
  os draws antes da thread da GPU leem as tabelas de recursos de cada shader; quando essa leitura
  antecipada pegava uma memória já reaproveitada, o tratador de falhas remendava o shader para
  sempre ler zero ali. Agora essa leitura só é descartada e o draw é preparado pela thread da GPU.
  Aqui (NVIDIA) isso nunca aconteceu, nem com o cache vazio; falta a confirmação em Intel.
- Com o modo de 60 FPS, o menu do jogo não diz mais que o patch de 60 FPS está indisponível (e o
  log não diz mais "30 FPS"): o jogo já rodava a 60, só a indicação estava errada.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.2:

- **Jogabilidade mais fluida em cenas pesadas**: no telhado de Londres logo depois do tutorial
  (~1.430 draws por quadro) o jogo rodava a 23–25 FPS; agora fica em 28–30 (o limite do jogo é
  30). Um quarto do tempo das threads da GPU era gasto esperando o lock do mapa de memória do
  jogo, que no Windows usava o rwlock lento do winpthreads; agora usa o SRWLOCK do próprio
  Windows. Cada draw custa 14–17 µs na thread da GPU em vez de 21.
- Diagnóstico para desenvolvedores: `THEORDER_SAMPLE=<nome da thread>` amostra onde as threads
  gastam tempo (`tools/sample_report.py` mostra o resultado por função), e a estatística de µs por
  draw passou a funcionar no Windows.
- O cache de shaders é mantido (sem recompilar depois de atualizar).

Novidades da 0.3.1:

- **Crash raro ao iniciar corrigido** (~1 em 40 inicializações): um componente do Windows
  (`iertutil.dll`) travava numa thread que o GIO, do GLib, abre ao ser carregado para listar os
  apps da Microsoft Store. O GIO só vinha junto com o decodificador de SVG do FFmpeg, que o jogo
  nunca usa. O pacote agora traz um `librsvg-2-2.dll` mínimo no lugar do original, e GIO, Pango e
  GDK-Pixbuf não são mais carregados.
- **Pausas menores ao carregar cenas**: os pipelines guardados (arquivos de ~2 MB) agora são lidos
  com stdio, cerca de 1,6× mais rápido (3,2–3,4 → 1,8–2,2 ms por pipeline, medido em execuções
  intercaladas); a maior pausa, no início do prólogo (~95 pipelines), caiu de ~490 para ~330 ms.
- Diagnóstico para desenvolvedores: `THEORDER_WATCH=<endereço>:<bytes>` registra cada mudança de
  um trecho da memória do jogo; os relatórios de crash mostram mais registradores e o estado da
  fila de tarefas do motor.
- Desta vez o cache de shaders não é refeito.

Novidades da 0.3.0:

- **Crash na jogabilidade corrigido**: os construtores estáticos do jogo rodavam duas vezes (o
  loader chamava o inicializador e o próprio `_start` do jogo também), e os pools de memória do
  motor (1,6 GB) eram alocados em dobro. Ao carregar o save no ponto do tutorial de cobertura, a
  memória direta acabava e o thread de streaming travava o jogo. Com o mesmo save, agora roda
  7 minutos com 4,2 de 5,3 GB em uso.
- **Jogabilidade "azul/estourada" corrigida**: depois do prólogo a imagem virava uma cor chapada
  azul-clara só com o HUD. Um vertex shader sem recursos reaproveitava uma variante compilada com
  outro deslocamento de user data e desenhava a limpeza de um buffer de neblina fora da tela; o
  buffer guardava a tela de carregamento e estourava a exposição.
- **Proporção da imagem** (launcher: Gráficos e configurações rápidas; menu do jogo: Imagem):
  Original (2,40:1 com faixas pretas), **16:9**, **16:10** ou **21:9** preenchem a tela sem
  faixas. A visão mantém a largura original e mostra mais em cima e embaixo (Vert+), e o port
  recorta as faixas que sobram em telas 16:10 e 21:9. Custo a 60 FPS no prólogo: ~1%.
- Shaders: as multiplicações "legacy" do GCN (0 × Inf = 0) e os rcp/rsq/log com clamp agora
  seguem o hardware. O cache de shaders é refeito uma vez na primeira sessão.
- Limpeza: tudo que restava do port de God of War III saiu (cheats, patches, fontes, ícones);
  o launcher só oferece opções que funcionam no The Order. As variáveis de ambiente passaram de
  `GOW3_*` para `THEORDER_*` e o arquivo de configurações de `gow3.ini` para `theorder.ini`
  (movido sozinho na primeira execução).

Problemas conhecidos:

- Vindo de uma versão anterior à 0.3.0, a primeira sessão refaz o cache de shaders: objetos podem
  aparecer com um instante de atraso e há pausas curtas.
- Raramente um triângulo preto pisca por um quadro. Os vértices na memória do jogo estão certos
  (o motor os escreve uma única vez); a causa está do lado do port, em investigação.
- Ainda não verificado: a maior parte da campanha depois do primeiro capítulo, som, placas AMD e
  Intel.

## Controles

Controle (DualSense, Xbox e outros, via SDL3) e teclado funcionam juntos; a tabela de teclas está
no README; o mouse gira a câmera e seus botões atiram e miram (F7 liga e desliga). **Insert** (ou R3+L2 no controle) abre o menu do port (tela,
limite de quadros e overlay de desempenho).

## Arquivos (código-fonte)

- `launcher/port_launcher_win.py`: o launcher (empacotado por `packaging/windows/package.sh`).
- `out/theorder-probe.exe`: o executável do port (gerado por `bash build.sh`).
- `user/savedata/`, `user/cache/CUSA00035/`: saves e cache de shaders e pipelines.
- `patches/TheOrder1886-Orbis.xml`: patches do jogo, todos desligados por padrão.
- `tools/batch_runs.py`, `tools/fault_stacks.py`: testes automatizados e análise de crashes.
