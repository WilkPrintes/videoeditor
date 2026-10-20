# fx/

Reservado para a **fase 3** da especificação (§6.5–6.7): transições (dissolve, dip, wipes, push),
transformação (posição, escala, rotação, crop, opacidade, modos de mesclagem em float linear),
títulos e mudança de velocidade com rampas e interpolação de frames.

O modelo já tem `Transition`, `Effect` com parâmetros animáveis (`AnimatedParam`) e `Retime` em
`core/Model.h`.
