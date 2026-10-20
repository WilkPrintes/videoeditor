# color/

Reservado para a **fase 4** da especificação (§8): grafo de nós de cor na GPU em ACEScct
(primárias, curvas, curvas de matiz, qualificador HSL, janelas com tracking, LUT 3D tetraédrica),
scopes por acumulação na GPU e OCIO para entrada e saída. As ideias vêm do protótipo
`C:\Users\Wilk\Documents\colornode`.

O modelo já guarda o grade de cada clipe (`Clip::grade`, em `core/Model.h`), e o viewer já
processa em float (RGBA16F) no passo "process" de `ui/ViewerWidget.cpp`.
