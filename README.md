para executar o trabalho entre na pasta em que ele foi salvo pelo terminal e digite:

gcc roteador.c -o roteador -lpthread
./roteador 1/2/3 ou 4 (abrir um terminal para cada roteador)

nesta etapa é possível que:
a) os roteadores conversem com todos os roteadores conhecidos após o bellman-ford.
b) enviem periodicamente seus vetores distância para seus vizinhos, com as atualizações do bellman-ford.

No terminal irá aparecer algo assim:

[Router 1] Vetor recebido do roteador 2:
rot   1   2   3   4
dist  3   0   2   10

ou seja, o roteador 1 recebeu as distâncias de 2.

depois "1" analisa se é viável atualizar alguma rota e avisa na tela, se mudou manda o vetor novo para os vizinhos, 
se não diz que não há atualizações.

para desativar um roteador digite "-1".
após desativado, um roteador manda para seus vizinhos as distâncias com INF e então ele fica INF para seus vizinhos, porém,
dependendo do caso, os viznhos acabam recalculando a rota para o vizinho "morto", pois a informação não chega até todos que o utilizavam 
como rota. 
