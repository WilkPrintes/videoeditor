# Arquitetura

## Camadas (spec §4)

```
 app/ (videoeditor)          tools/ve-cli
        │                         │
 ui/ (Qt Widgets + QRhi viewer)   │        tests/ui (Qt Test)
        │ Commands / gestos       │
 engine/ (reprodução, export) ◄───┘
        │
 audio/ (miniaudio, mixer)
        │
 media/ (FFmpeg: probe, decoders, MP4 writer)
        │
 core/ (tempo, modelo, edições, transações, JSON)   ◄── tests/core (Catch2)
```

- **core** não depende de Qt nem de FFmpeg. É o modelo, as regras de edição, o desfazer e a
  serialização, 100% testável isoladamente.
- **media/audio/engine** não dependem de Qt. São o I/O de mídia e o tempo real (reprodução e
  exportação), reaproveitáveis num render headless ou num processo separado.
- **ui** só lê o modelo e emite *pedidos* (mover, trim, cortar...). A `MainWindow` transforma
  cada pedido em Command. Nenhum widget altera o modelo diretamente.
- `fx/`, `color/` e `analysis/` estão reservadas para as fases 4, 5 e 9.

## Tempo (spec §5.1)

- Toda posição e duração é um inteiro em **flicks** (`ve::Time`, 1/705 600 000 s). O flick divide
  exatamente a duração de frame de 23,976/24/25/29,97/30/48/50/59,94/60/120 fps e a de amostra de
  44,1/48/88,2/96/192 kHz, então nada acumula erro.
- Frames e segundos só existem nas bordas: `frameAt`, `timeOfFrame`, `sampleAt` e
  `timeOfSample`. Todas usam aritmética inteira exata (`mulDivFloor`), e a regra de janela é
  sempre a mesma: um instante pertence ao frame cuja janela o contém.
- As edições mantêm os clipes na grade de frames da timeline. A mídia com outra cadência é
  conformada pelo frame mais próximo na decodificação.

## Modelo de dados (spec §5.2, inspirado no OpenTimelineIO)

```
Project ─ MediaPool ─ Folder* ─ MediaItem (arquivo, streams, timecode, taxa, metadados, proxy)
        └ Timeline* ─ Track* (Video | Audio, lock, sync lock) ─ Item*
                    └ Marker* (início, duração, nome, nota, cor)
Item = Clip(mediaId, sourceRange, enabled, linkGroup, color, flags[], effects[], grade, retime)
     | Gap | Transition(in/out, alinhamento) | CompoundClip(timelineId) | MulticamClip(angles[], activeAngle)
```

- Como no OTIO, a trilha é uma **sequência**: o início de um item na timeline é a soma das durações
  anteriores, e o espaço vazio é um `Gap` explícito. Assim, sobreposição na mesma trilha é
  impossível por construção. Transições não ocupam tempo.
- Trilhas de um mesmo tipo têm ordem: V2 fica acima de V1 na composição e A2 vem depois de A1.
  `edit::kindIndex` e `edit::trackOfKind` convertem entre trilha e número.
- `sourceRange.start` é o tempo de mídia do primeiro frame; `sourceRange.duration` é quanto o item
  ocupa na timeline. Um `Retime` (velocidade constante num/den) funciona como o `LinearTimeWarp` do
  OTIO: o clipe lê `sourceOffset(item, t)` de mídia em `t` de timeline. Toda conta de mídia (fatiar,
  handles, slip, render) passa por `sourceOffset`, `clipTimeFor` e `sourceEnd`.
- `AnimatedParam` é um valor fixo ou uma curva de keyframes (linear e hold; Bézier na fase 4).
- **Clipes ligados:** vídeo e áudio importados juntos compartilham um `linkGroup` e editam juntos.
  Quando uma edição parte clipes em várias trilhas ao mesmo tempo, `IdRemap` dá às metades novas
  um grupo novo, igual entre as trilhas.
- As primitivas da trilha ficam em `Track`:
  - `liftRange` e `placeItem` (overwrite);
  - `splitAt` e `normalize`;
  - `insertSpace` e `removeRange` (abrir e fechar tempo, a base dos ripples).

  As operações de usuário ficam em `core/Edit.h`; a seção [Edições da fase 2](#edições-da-fase-2)
  descreve as regras.
- `edit::checkInvariants` define o que é uma timeline válida: grade de frames, handles de mídia
  (com a velocidade), ids únicos, marcadores ordenados e nenhum gap sobrando. `edit::checkSync`
  confere a sincronia dos clipes ligados. Os testes de propriedade aplicam as duas a cada edição.

## Comandos, transações e desfazer (spec §5.3)

No Resolve, as edições aparecem como famílias `*Act` com `CreateTransaction/Commit/Abort`. Aqui:

- `Command{redo, undo, mergeWith}` empilhados no `CommandStack`, que guarda o índice "limpo"
  (salvo) e notifica a UI.
- `TimelineEditCommand` executa a operação **uma vez** e guarda os snapshots antes e depois da
  timeline. Undo e redo só trocam snapshots, então nenhuma edição precisa de lógica inversa. Se
  a operação recusa no meio, o snapshot é restaurado: cada operação é **atômica**.
- **Transações:** `beginTransaction` / `commit` / `abort`. Tudo o que é empurrado entre elas
  vira um único passo de desfazer, e `abort` volta ao estado anterior.
- **Gestos:** um arraste na timeline abre uma transação no press e empurra um comando por
  movimento do mouse, com a mesma `mergeKey`. Comandos com a mesma chave **se fundem**: o anterior
  é desfeito e o novo é aplicado a partir do estado do início do gesto. A timeline e o viewer
  acompanham o mouse ao vivo, e passar por cima de um clipe e voltar não o destrói. O release
  faz commit; Esc faz abort.
- Comandos que não mudam nada (corte onde já há corte, move de 0 frame) são descartados.
- `ProjectEditCommand` é o mesmo esquema para edições que criam ou apagam timelines (clipes
  compostos). Os snapshots guardam todas as timelines do projeto e a corrente; o media pool fica de
  fora.

## Edições da fase 2

Funções de `core/Edit.h`, sem UI e testadas isoladamente. Regras comuns:

- **Validadas e atômicas:** tudo é conferido antes de mexer no modelo, então uma edição recusada
  (retorno `false` ou vazio) não muda nada, mesmo chamada fora de um comando. Os trims
  interativos limitam o delta ao possível em vez de recusar.
- **Trava:** nenhuma edição altera uma trilha travada. Um destino travado recusa a edição. Clipes
  em trilha travada não podem ser escolhidos na UI.
- **Sync lock:** edições que acrescentam ou tiram tempo deslocam também as trilhas com sync lock, e
  não só as editadas. Isso vale para insert, ripple trims e ripple delete. Uma trilha com sync lock
  sempre aceita tempo vazio: o clipe que atravessa o ponto é cortado, como no insert do Premiere.
  Mas ela só cede tempo vazio, como no Avid: se tiver mídia no trecho que sairia, a edição é
  limitada (trims) ou recusada (ripple delete). Para deixar uma trilha de música parada, desliga-se
  o sync lock dela.

### Três e quatro pontos (§6.1)

`EditPoints` traz os quatro pontos (In/Out da fonte, In/Out da timeline), o playhead e as trilhas
de destino. `resolvePoints` deduz os que faltam:
- **duração:** In→Out da timeline, ou In→Out da fonte;
- **início na fonte:** o In da fonte, ou o Out da fonte menos a duração (backtiming);
- **início na timeline:** o In da timeline, o Out da timeline menos a duração, ou o playhead.

Com os quatro pontos, vale a faixa da timeline. As edições:
- `insertEdit` abre espaço nos destinos e nas trilhas com sync lock;
- `overwriteEdit` não desloca nada;
- `replaceEdit` troca o clipe sob o ponto mantendo lugar e duração, com o In da fonte alinhado ao
  ponto;
- `fitToFillEdit` calcula a velocidade `quadros da fonte / quadros da timeline`, reduzida pelo mdc;
- `placeOnTopEdit` usa a primeira trilha livre acima de todos os clipes do trecho e cria trilhas
  se faltar.

### Trims (§6.2)

Cada trim pega a borda do clipe e, salvo pedido contrário, a mesma borda dos parceiros ligados. É
no máximo uma por trilha, e cada parceiro usa a sua própria posição, o que preserva cortes J/L já
existentes. O delta é limitado pelos handles de mídia, pelo mínimo de 1 frame e pelos vizinhos.

| Trim | Implementação |
|---|---|
| `rippleTrim` | em cada trilha com borda: `insertSpace` + clipe estendido, ou `removeRange` do pedaço que sai. Nas outras trilhas com sync lock, o mesmo tempo entra ou sai no ponto da borda principal. Um ripple de cabeça mantém o início do clipe e desloca o conteúdo |
| `rollTrim` | move o corte: cauda do item da esquerda e cabeça do da direita; um gap vizinho absorve |
| `slipClip` | `sourceRange.start += sourceOffset(delta)` no clipe e nos parceiros |
| `slideClip` | o clipe anda entre os vizinhos, que cedem ou ganham tempo |
| `rippleDelete` | as trilhas com sync lock perdem a união dos trechos apagados, e só pode haver clipes selecionados ou vazio neles; trilhas sem sync lock fecham só os próprios buracos |

Com `linked = false` (Alt na UI), roll e ripple ajustam só a trilha clicada: é assim que se fazem
cortes J e L.

**Sincronia.** Dois clipes ligados estão em sincronia quando levam o tempo da timeline ao tempo
de mídia do mesmo jeito: mesma velocidade e mesmo deslocamento (`checkSync`). Um corte J/L continua
em sincronia, porque só as bordas diferem. Ripples e inserts deslocam "tudo a partir de um ponto"
em cada trilha, então `planSync` classifica cada clipe ligado como antes do ponto, depois do ponto
ou atravessando o ponto:
- se um mesmo take ficaria com membros antes e depois, a edição é recusada;
- se o take tem uma parte atravessando o ponto e outra depois, quem fica com o grupo é a metade que
  andou junto (`applySync`). Isso cobre, por exemplo, um insert dentro da sobreposição de um
  corte L.

Fora isso, só uma trilha travada ou sem sync lock pode tirar um take de sincronia, e isso é escolha
do usuário: `outOfSyncClips` alimenta a faixa vermelha na timeline. O teste de propriedade confere
a sincronia em todas as edições enquanto nenhuma trilha está travada ou sem sync lock.

### Clipes compostos (§6.3)

- **Compor:** `composeClips` cria uma `Timeline` no projeto com as trilhas entre a mais baixa e a
  mais alta selecionada de cada tipo, para não mudar o empilhamento. Os clipes mantêm id e grupo, e
  no pai entra um `CompoundClip` por tipo, ligados entre si. A seleção é recusada se um clipe não
  selecionado dessas trilhas cruzar o trecho.
- **Decompor:** `decomposeClip` põe de volta o pedaço da filha que o composto mostra, da trilha
  dele para cima (criando trilhas se faltar). A timeline filha é apagada quando ninguém mais aponta
  para ela, então compor e depois decompor dá exatamente o mesmo projeto. Se outro pedaço do
  composto ainda usa a filha, os clipes recolocados ganham ids e grupos novos.
- **Render:** o `TimelineSnapshot` leva as timelines aninhadas.
  - **Vídeo:** `resolveVideo` desce nos compostos; onde a filha está vazia, aparece o que está
    abaixo.
  - **Áudio:** `audioSegments` achata o áudio dos compostos na ordem das trilhas, que é a ordem da
    soma no mixer. Por isso o resultado é idêntico bit a bit.
  - **Prova:** o exportador tem `renderHash`, um hash de cada frame e amostra antes do encoder. O
    `ve-cli selftest` exporta antes e depois de compor e compara os hashes.

### Marcadores (§6.8) e a timeline na UI

Os marcadores são da timeline, ordenados e um por frame. Ripples não os movem. No JSON eles usam
`marked_range`, como no OTIO. Na `TimelineWidget`:
- duas ferramentas (Seleção e Trim) e modificadores: Shift faz ripple, Ctrl faz roll e Alt edita só
  a trilha clicada;
- cada arraste é um gesto transacional, como o move e o trim da fase 1;
- o snapping usa cortes, marcadores, playhead e In/Out, com tolerância em pixels convertida para
  frames;
- o cabeçalho de cada trilha escolhe o destino das edições de 3 e 4 pontos e liga a trava e o sync
  lock (comandos com desfazer).

### Estados dourados e propriedades

- **Dourados:** `tests/core/test_golden.cpp` monta uma timeline de 4 trilhas com
  `ScopedIdSequence`, que dá ids determinísticos. Para cada operação, grava o antes e o depois
  (`timelineToJson`) em `tests/core/golden/<nome>.json`, com um resumo em frames para revisão.
  `VE_UPDATE_GOLDEN=1` regrava os arquivos.
- **Propriedade:** `tests/core/test_property.cpp` roda 2 × 22 500 edições aleatórias (cinco
  sementes de 4 500) de todos os tipos pelo `CommandStack`, inclusive as da fase 3:
  - uma rodada só com edições ligadas, que exige alinhamento V/A estrito;
  - outra com J/L, mídia de um tipo só e travas.

  A cada passo valem `checkInvariants`, `checkSync`, trilhas travadas intactas e, no fim, o undo
  total.

## Fase 3: organização, edições novas e a janela

### Organização e edições novas (core)

- Clipes têm `color` e `flags` (ordenadas, sem repetição); trilhas têm `color`. Nomes de trilha e de
  timeline são únicos. O JSON só grava esses campos quando diferem do padrão, então projetos e
  dourados anteriores continuam idênticos byte a byte.
- `linkClips` junta clipes (um por trilha) num grupo novo; `unlinkClips` tira do grupo e desfaz o
  grupo que ficar com um clipe só. Um link fora de sincronia continua editando junto e aparece em
  `outOfSyncClips`.
- **Linked selection desligado:** `splitAt`, `moveClips`, `deleteClips` e `rippleDelete` recebem
  `linked = false` e editam só os clipes pedidos, como no Resolve. Os trims já tinham o Alt.
- `appendEdit` vai depois do último item das trilhas de destino. `rippleOverwriteEdit` é um ripple
  delete do clipe sob o ponto seguido de um insert, atômico (trabalha numa cópia) e com as mesmas
  regras de sync lock.
- `insertGap` abre tempo vazio em todas as trilhas com sync lock; `deleteGap` fecha um gap (nas
  outras trilhas com sync lock o trecho precisa estar vazio); `deleteGaps` fecha todo trecho vazio
  em todas elas. `swapClips` troca um clipe (e os ligados) com o vizinho; os ligados precisam trocar
  com os ligados do vizinho, para a sincronia não quebrar.
- Várias timelines: `newTimeline`, `duplicateTimeline`, `renameTimeline`, `deleteTimeline` (recusado
  para a última e para uma que um composto mostra) e `setCurrentTimeline`.
- Testes: 7 dourados novos (29 no total; os 22 da fase 2 não mudaram), casos em
  `test_organization.cpp` e `test_gaps_and_order.cpp` e as edições novas no teste de propriedade.

### A janela (ui)

- O widget central é o viewer da timeline. Media pool e edit index (em abas), viewer de fonte,
  inspector e timeline são `QDockWidget`s. A timeline ocupa a largura toda (`setCorner`). Cada dock
  tem `objectName`, então `saveState`/`restoreState` guardam o layout: os presets ficam em
  `layouts.ini` (`QSettings`, formato ini, em `AppConfigLocation`). O último layout é gravado ao
  fechar e restaurado só pelo `app/main.cpp`; os testes ligam o modo de teste do `QStandardPaths` e
  começam do layout padrão.
- **Dois viewers, um ativo.** O viewer de fonte (`SourceViewerPanel`) tem seu próprio
  `PlaybackEngine`, sobre um snapshot de um clipe só com a mídia na grade de frames da timeline. Um
  clique num viewer ou na timeline escolhe o ativo, e Q alterna. Transporte, Home/End, setas e I/O
  vão para o ativo, e tocar um pausa o outro. As edições de 3 e 4 pontos usam a mídia do viewer de
  fonte com o In/Out dela; com o viewer vazio, a mídia selecionada no pool, inteira.
- Os painéis nunca editam o modelo. `InspectorWidget` e `EditIndexWidget` emitem sinais, e a
  `MainWindow` transforma cada um num comando (`TimelineEditCommand` ou `ProjectEditCommand`). Os
  menus de contexto saem de `MainWindow::contextMenu(TimelineWidget::Context)`, separado do `exec`
  para os testes acionarem as ações.
- Abas de timeline: uma por timeline do projeto, compostos incluídos. Trocar de aba não é uma
  edição (cada comando lembra o id da sua timeline, então o desfazer continua certo), e cada
  timeline guarda o próprio playhead.
- O `QWidget` apaga os filhos depois dos membros; por isso o destrutor da `MainWindow` desconecta
  os sinais dos painéis antes.

## Persistência

`core/ProjectFile` gera JSON no estilo OTIO (objetos com `schema`, trilhas com `children`, marcadores
com `marked_range`), com tempos em flicks (formato 2; os marcadores são opcionais, então projetos
anteriores continuam abrindo). Os caminhos da mídia são guardados absolutos e relativos ao projeto,
para religar se a pasta for movida. O JSON é próprio do core (`core/Json`): sem dependências,
independente de locale e com int64 exato. A gravação é atômica (arquivo temporário + rename).
Um banco SQLite com autosave incremental fica para a fase de persistência (spec §11).

## Motor de reprodução

```
 UI (thread principal, timer de 8 ms)        thread de vídeo                 thread de áudio
 ─────────────────────────────────────       ─────────────────────────────   ─────────────────────────
 setTimeline(snapshot) ───────────────────►  decodifica frames à frente      mixa A1..An em blocos de
 play / pause / seek                         numa fila (8 frames), ou          1024 amostras → ring SPSC
 currentVideoFrame()  ◄── fila / still       atende o último pedido de        (lock-free)
 position()  ◄── relógio                     scrub (o mais recente vence)              │
                                                                             callback do dispositivo
                                                                             (WASAPI / Core Audio)
                                                                             consome o ring:
                                                                             amostras tocadas = RELÓGIO
```

- **O áudio é o relógio mestre.** A posição é `início + (amostras_tocadas − latência) / taxa`.
  Só amostras reais contam: num underrun o relógio para em vez de dessincronizar. Sem dispositivo
  de áudio, o relógio usa `steady_clock`.
- O vídeo é **apresentado pelo relógio**. A cada tick, a UI pega o frame mais recente com
  `frame ≤ relógio` e descarta os atrasados. Se o decoder ficar para trás, a fila pula adiante.
- **Scrubbing com precisão de frame:** `VideoDecoder::frameAt(t)` busca o keyframe anterior e
  decodifica até o frame mais próximo de `t`. Pedidos sequenciais não fazem seek. Pedidos de
  scrub se juntam, e só o mais recente é decodificado.
- **Hardware:** D3D11VA no Windows e VideoToolbox no macOS. O frame é baixado para 4:2:0
  planar, 8 ou 16 bits. Se o hardware não suportar o perfil, cai para software.
- **Snapshots imutáveis:** a UI cria um `TimelineSnapshot` (timeline, timelines aninhadas e mídias)
  a cada mudança. As threads do motor nunca leem o modelo vivo.
- O mixer recebe de `audioSegments` o áudio já achatado (compostos expandidos, um decoder por
  caminho de trilha). Clipes com velocidade diferente de 1 ficam mudos até o time-stretch (§6.5).

## Viewer na GPU (Qt RHI)

```
 planos Y/U/V (R8 ou R16) ──► passo 1 "process" ──► RGBA16F (float) ──► passo 2 "display" ──► widget
                              YUV→RGB (matriz e faixa                    ajuste de aspecto
                              em uniforms)
```

- `QRhiWidget` usa D3D11 por padrão no Windows (D3D12 com `VE_RHI=d3d12`) e Metal no macOS. Os
  shaders são GLSL 440, compilados pelo `qsb` para HLSL, MSL e SPIR-V no build.
- O alvo RGBA16F intermediário é onde o grafo de efeitos e cor vai entrar (ACEScct, LUTs,
  nós), já em float, como no Resolve.

## Exportação

`exportTimeline()` percorre a timeline frame a frame:
- **Vídeo:** pega o clipe de cima (ou preto), descendo nos compostos, redimensiona com letterbox
  via swscale e passa ao encoder H.264.
- **Áudio:** passa pelo mesmo `AudioMixer` da reprodução, com amostras exatas por frame, e vai
  para o AAC.
- **Saída:** MP4 com `+faststart`.
- **`renderHash`** (opcional): um hash de cada frame e de cada amostra antes do encoder, para
  comparar dois renders exatamente, seja qual for o encoder.

Ordem dos encoders no modo automático:
- **Windows:** NVENC → AMF → Quick Sync → x264 (só em build GPL) → Media Foundation (MFT de
  hardware primeiro) → OpenH264.
- **macOS:** VideoToolbox → x264 → OpenH264.

Encoders do Media Foundation exigem COM MTA, então todo encode roda numa thread própria.

## Paralelos com o Resolve (análise estática) e o que ficou para depois

O mapa completo está no §14 da [especificação](ESPECIFICACAO.md).

| Resolve | Aqui |
|---|---|
| UI em Qt Widgets + toolkit desenhado na GPU | Qt Widgets + `QRhiWidget` |
| Páginas como bibliotecas carregadas sob demanda | ainda não: janela única com a página Edit; barra de páginas na fase 7 |
| `UiPanelLayoutBase` (painéis e layouts por página) | `QDockWidget`s com presets de layout em `QSettings` |
| Modelo Sm2Timeline/Track/Clip/Gap/Transition/Compound/Multicam em protobuf num banco SQLite/PostgreSQL | modelo OTIO-like (com compostos e multicam) em JSON com flicks; SQLite é o próximo passo |
| Ações *Act com undo e CreateTransaction/Commit/Abort | `Command` + `CommandStack` com transações e fusão de gestos |
| Modo de trim (ripple/roll/slip/slide), edições F9–F12, sync lock e travas de trilha | `core/Edit.h` com as mesmas edições; ferramentas e modificadores na timeline |
| Sm2MpCompoundClip (timeline aninhada como fonte) | `CompoundClip` + timeline no projeto; o snapshot renderiza a filha |
| Gerentes com filas de mensagens e planejadores de tarefas | threads de vídeo e áudio com filas; snapshot por mudança |
| Grafo de nós na GPU em float, kernels compilados em runtime | passo float RGBA16F pronto; o grafo vem depois |
| FFmpeg + SDKs de câmera | FFmpeg (LGPL); o host de plug-ins de codec fica para depois |
| Host OFX, scripting Python/Lua | fora do marco 1 |
