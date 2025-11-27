#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>

#define MAX_MSG 100
#define MAX_FILA 100
#define MAX_ROUTERS 10
#define INF 1000000000

typedef struct {
    int type; // 0 = controle (vetor), 1 = dados
    int rot_fonte;
    int rot_destino; 
    int dist[MAX_ROUTERS]; // usado em mensagens de controle
    char payload[MAX_MSG]; // usado em mensagens de dados
} Mensagem;

/* fila bloqueante */
typedef struct {
    Mensagem itens[MAX_FILA];
    int frente, tras, size;
    pthread_mutex_t mutex; // mutex para proteger a fila
    pthread_cond_t not_empty; // condição para fila não vazia
    pthread_cond_t not_full; // condição para fila não cheia
} Fila;

void fila_init(Fila *f) { // inicializa a fila
    f->frente = 0;
    f->tras = 0;
    f->size = 0;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->not_empty, NULL);
    pthread_cond_init(&f->not_full, NULL);
}

int vizinho[MAX_ROUTERS][MAX_ROUTERS]; // 1 se são vizinhos, 0 caso contrário
int custo_enlace[MAX_ROUTERS][MAX_ROUTERS]; // matriz de custos

void read_enlaces_config(const char *fname) {
    FILE *f = fopen(fname, "r");
    if (!f) { perror("Erro abrindo enlaces.config"); exit(1); }

    for (int i = 0; i < MAX_ROUTERS; i++) // inicializa matrizes vizinhos = o e enlaces = inf
        for (int j = 0; j < MAX_ROUTERS; j++) {
            vizinho[i][j] = 0;
            custo_enlace[i][j] = INF; // custo infinito inicialmente
        }

    int a, b, custo;
    while (fscanf(f, "%d %d %d", &a, &b, &custo) == 3) { // define vizinhos e custos
        vizinho[a][b] = 1;
        vizinho[b][a] = 1; // bidirecional
        custo_enlace[a][b] = custo;
        custo_enlace[b][a] = custo;
    }
    fclose(f);
}


void enfileirar(Fila *f, Mensagem m) { // adiciona item na fila
    pthread_mutex_lock(&f->mutex);
    while (f->size == MAX_FILA)    // fila cheia
        pthread_cond_wait(&f->not_full, &f->mutex); // espera fila não cheia
    f->itens[f->tras] = m; // insere item no tras
    f->tras = (f->tras + 1) % MAX_FILA; // avança tras
    f->size++; // incrementa tamanho
    pthread_cond_signal(&f->not_empty); // sinaliza que fila não está vazia
    pthread_mutex_unlock(&f->mutex); // libera mutex
}

Mensagem desenfileirar(Fila *f) { // remove item da fila
    pthread_mutex_lock(&f->mutex);
    while (f->size == 0)
        pthread_cond_wait(&f->not_empty, &f->mutex); // espera fila não vazia
    Mensagem m = f->itens[f->frente]; // pega item da frente
    f->frente = (f->frente + 1) % MAX_FILA; // avança frente
    f->size--; // decrementa tamanho
    pthread_cond_signal(&f->not_full); // sinaliza que fila não está cheia
    pthread_mutex_unlock(&f->mutex); // libera mutex
    return m;
}

/* --- globais --- */
int meu_id;
int sockfd;
int port_map[MAX_ROUTERS];         
int exists_router[MAX_ROUTERS];    
int num_known_routers = 0;

Fila filaEntrada;
Fila filaSaida;

int dist_table[MAX_ROUTERS];       
int next_hop[MAX_ROUTERS];         

pthread_mutex_t rt_mutex = PTHREAD_MUTEX_INITIALIZER;

/* pega sockaddr_in para dado id */
struct sockaddr_in getEndereco(int id) {
    struct sockaddr_in addr; // endereço do roteador
    memset(&addr, 0, sizeof(addr)); // limpa memoria
    addr.sin_family = AF_INET; // IPv4
    addr.sin_port = htons(port_map[id] > 0 ? port_map[id] : 5000 + id); // porta
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // localhost
    return addr;
}

/* --- envia vetor para vizinhos */
void enviar_vetor_distancias() {
    Mensagem m;
    memset(&m, 0, sizeof(Mensagem)); // limpa mensagem
    m.type = 0; // tipo 0 = controle
    m.rot_fonte = meu_id; // id do roteador de origem

    pthread_mutex_lock(&rt_mutex);
    for (int i = 0; i < MAX_ROUTERS; i++)
        m.dist[i] = dist_table[i];
    pthread_mutex_unlock(&rt_mutex);

    for (int v = 0; v < MAX_ROUTERS; v++) {
        if (vizinho[meu_id][v]) {
            m.rot_destino = v;
            enfileirar(&filaSaida, m);
        }
    }
}

// reiver recebe mensagens udp de outros roteadores
void* receiver(void* arg) {
    Mensagem msg;
    struct sockaddr_in remetente; // endereço do remetente
    socklen_t len = sizeof(remetente); // tamanho do endereço
    while (1) {
        ssize_t n = recvfrom(sockfd, &msg, sizeof(Mensagem), 0, (struct sockaddr*)&remetente, &len); //recebe mensagem
        if (n <= 0) continue;  // erro ou nada recebido
        enfileirar(&filaEntrada, msg); // coloca msg recebida na fila de entrada
        printf("[Router %d] Mensagem recebida (tipo %d) de %d\n", meu_id, msg.type, msg.rot_fonte);
    }
}

// sender envia mensagens da fila de saída via udp
void* sender(void* arg) {
    while (1) {
        Mensagem msg = desenfileirar(&filaSaida);

        // 1) MENSAGENS DE CONTROLE - ENVIA SÓ A VIZINHOS 
        if (msg.type == 0) {
            int target = msg.rot_destino;

            if (!vizinho[meu_id][target]) {
                continue;
            }

            struct sockaddr_in addr = getEndereco(target);
            sendto(sockfd, &msg, sizeof(Mensagem), 0,
                   (struct sockaddr*)&addr, sizeof(addr));

            printf("[Router %d] Mensagem de controle enviada ao vizinho %d.\n",
                   meu_id, target);

            usleep(1000);
            continue;
        }

        // 2) MENSAGENS DE DADOS - USAM NEXT-HOP          
        if (msg.type == 1) {
            int destino_final = msg.rot_destino;

            pthread_mutex_lock(&rt_mutex);
            int nh = next_hop[destino_final];
            int dist = dist_table[destino_final];
            pthread_mutex_unlock(&rt_mutex);

            if (nh == -1 || dist >= INF) {
                printf("[Router %d] Sem rota para %d — mensagem descartada.\n",
                    meu_id, destino_final);
                continue;
            }

            struct sockaddr_in addr = getEndereco(nh);
            sendto(sockfd, &msg, sizeof(Mensagem), 0,
                (struct sockaddr*)&addr, sizeof(addr));

            printf("[Router %d] Mensagem encaminhada para %d via next-hop %d.\n",
                meu_id, destino_final, nh);

            usleep(1000);
            continue;
        }

    }
}


void bellman_ford(int vizinho_id, int *dist_vizinho) {
    pthread_mutex_lock(&rt_mutex);

    int alterou = 0;

    printf("\n[Router %d] --- Processando vetor do vizinho %d ---\n", meu_id, vizinho_id);

    int cost_to_neighbor = custo_enlace[meu_id][vizinho_id];

    // verifica se vizinho morreu -----------------------------------------------------------------------
    int todos_inf = 1;
    for (int k = 1; k < MAX_ROUTERS; k++) {   
        if (dist_vizinho[k] < INF) {       
            todos_inf = 0;
            break;
        }
    }

    if (todos_inf) {
        printf("[Router %d] Vizinho %d anunciou INF para todos (morto). Removendo rotas.\n",
               meu_id, vizinho_id);

        // marca custo do enlace como INF e remove vizinhança
        custo_enlace[meu_id][vizinho_id] = INF;
        custo_enlace[vizinho_id][meu_id] = INF;
        vizinho[meu_id][vizinho_id] = 0;
        vizinho[vizinho_id][meu_id] = 0;

        // remove o próprio vizinho como destino
        dist_table[vizinho_id] = INF;
        next_hop[vizinho_id] = -1;
        alterou = 1;
        printf("   -> Removendo rota direta para o vizinho morto %d\n", vizinho_id);

        for (int j = 1; j < MAX_ROUTERS; j++) {
            if (next_hop[j] == vizinho_id) { //se o morto era o next hop
                printf("   -> Removendo rota para %d via %d\n", j, vizinho_id);
                dist_table[j] = INF;
                next_hop[j] = -1;
            }
        }
        for (int j = 1; j < MAX_ROUTERS; j++) {
            int i = next_hop[j];
            if (next_hop[i] == vizinho_id) { //se o morto era o next hop do next hop
                printf("   -> Removendo rota para %d via %d\n", j, vizinho_id);
                dist_table[j] = INF;
                next_hop[j] = -1;

            }
        }

        pthread_mutex_unlock(&rt_mutex);

        if (alterou) enviar_vetor_distancias();
        return;
    }
    //----------------------------------------------------------------------------------------------------
    //BELLMAN-FORD:


    for (int j = 1; j < MAX_ROUTERS; j++) { //4-maxrourters
        if (!exists_router[j])continue;   // ignora IDs não existentes
        if (j == meu_id) continue; // ignora a si mesmo

        int dist_from_neighbor = dist_vizinho[j]; //distancia que ele recebeu do vizinho
        int nova_dist = cost_to_neighbor + dist_from_neighbor;
        if (nova_dist >= INF) nova_dist = INF;

        printf("[Router %d] Dest %d |  dist_vizinho=%d  nova_dist=%d  dist_table=%d\n",
               meu_id, j, cost_to_neighbor, dist_from_neighbor, nova_dist, dist_table[j]);

        if ((int)nova_dist < dist_table[j]) {
            printf("   -> Atualizando destino %d: %d -> %d (via %d)\n", j, dist_table[j], nova_dist, vizinho_id);

            dist_table[j] = nova_dist;

            printf("   -> destino atualizado %d: %d -> %d (via %d)\n", j, dist_table[j], nova_dist, vizinho_id);
            next_hop[j] = vizinho_id; //rota p chegar no id j é via vizinho_id
            alterou = 1;
        } else {
            printf("   -> Sem atualizacao para %d\n", j);
        }
    }

    // preparar mensagem de debug da tabela se alterou
    if (alterou) {
        printf("\n[Router %d] Tabela ATUALIZADA (após processar %d):\n\n", meu_id, vizinho_id);
    } else {
        printf("[Router %d] Nenhuma atualizacao apos processar %d\n", meu_id, vizinho_id);
    }

    pthread_mutex_unlock(&rt_mutex);

    if (alterou) {
        // disparar atualização imediata para vizinhos
        enviar_vetor_distancias();
    }
}


// packet_handler processa mensagens recebidas
void* packet_handler(void* arg) {
    while (1) {
        Mensagem msg = desenfileirar(&filaEntrada); // pega mensagem da fila de entrada

        if (msg.type == 0) { // vetor de distâncias recebido
            printf("\n[Router %d] Vetor recebido do roteador %d:\n", meu_id, msg.rot_fonte);

            // roteadores
            printf("rot   ");
            for (int i = 1; i <= 4; i++) {   // sempre mostra 1,2,3,4
                printf("%d   ", i);
            }
            printf("\n");

            // distâncias
            printf("dist  ");
            for (int i = 1; i <= 4; i++) {
                if (msg.dist[i] >= INF) //se infinito
                    printf("INF ");
                else
                    printf("%d   ", msg.dist[i]);  // mostra distância
            }
            printf("\n");
            
            //BELLMAN-FORD
            bellman_ford(msg.rot_fonte, msg.dist);

        } else if (msg.type == 1) { // mensagem de dados
            if (msg.rot_destino == meu_id) {
                printf("[Router %d] Mensagem entregue de %d: %s\n",
                    meu_id, msg.rot_fonte, msg.payload);
            } else {
                // reencaminhar para próximo salto
                enfileirar(&filaSaida, msg);
            }
        }
    }
    return NULL;
}


void* envia_dist(void *arg) { // envia vetor de distâncias periodicamente
    while (1) {
        sleep(10); //10 segundos para não ficar muito poluido o terminal
        enviar_vetor_distancias();
    }
    return NULL;
}

void* terminal_thread(void *arg) { // interface do usuário
    while (1) {
        char buffer[MAX_MSG]; // buffer para mensagem digitda
        int destino;
        printf("\nDigite destino (id) ou -1 para sair: ");

        if (scanf("%d", &destino) != 1) { //le destino e verifica se entrada é válida
            int c; 
            while ((c=getchar())!= '\n' && c!=EOF); // limpa entrada
            continue; 
        }

        if (destino == -1) {
            printf("[Router %d] Encerrando por comando do usuário.\n", meu_id);

            // 1) marca todas as distâncias locais como INF (sob mutex)
            pthread_mutex_lock(&rt_mutex);
            for (int i = 1; i < MAX_ROUTERS; i++) {
                dist_table[i] = INF;
                next_hop[i] = -1;
            }
            pthread_mutex_unlock(&rt_mutex);

            // 2) envia atualização com INF para TODOS os vizinhos
            enviar_vetor_distancias();

            // dar tempo para a fila de saída propagar as mensagens
            usleep(500000); // 0.5s

            // 3) remover enlaces localmente (impede novas comunicações diretas)
            pthread_mutex_lock(&rt_mutex);
            for (int v = 1; v < MAX_ROUTERS; v++) {
                custo_enlace[meu_id][v] = INF;
                custo_enlace[v][meu_id] = INF;
                vizinho[meu_id][v] = 0;
                vizinho[v][meu_id] = 0;
            }
            pthread_mutex_unlock(&rt_mutex);

            printf("[Router %d] Vetores INF enviados. Encerrando.\n", meu_id);
            close(sockfd);
            exit(0);
        }


        getchar();
        printf("Digite a mensagem (max %d chars): ", MAX_MSG-1);

        if (!fgets(buffer, MAX_MSG, stdin)) continue; // lê mensagem e verifica se válida
        buffer[strcspn(buffer, "\n")] = 0; // remove \n

        Mensagem m;
        memset(&m, 0, sizeof(Mensagem)); // limpa mensagem
        m.type = 1; // tipo 1 = dados 
        m.rot_fonte = meu_id;
        m.rot_destino = destino;
        strncpy(m.payload, buffer, MAX_MSG-1); // copia mensagem para payload
        enfileirar(&filaSaida, m); // coloca na fila de saída
    }
    return NULL;
}

/////configs/////
void read_roteador_config(const char *fname) {  // lê arquivo de configuração dos roteadores
    FILE *f = fopen(fname, "r"); // abre arquivo de configuração
    if (!f) { perror("Erro abrindo roteador.config"); exit(1); }
    for (int i = 1; i < MAX_ROUTERS; ++i) { // inicializa arays de portas e existência
        port_map[i] = -1;  // porta inválida default
        exists_router[i] = 0; // não existe
    }
    int id, port;
    while (fscanf(f, "%d %d %*s", &id, &port) == 2) {
        if (id > 0 && id <= MAX_ROUTERS) {
            port_map[id] = port;
            exists_router[id] = 1;
            num_known_routers++;
        }
    }
    fclose(f);
}

// inicializa tabelas de roteamento
void init_routing_tables() {
    pthread_mutex_lock(&rt_mutex);
    for (int i = 1; i < MAX_ROUTERS; ++i) { // inicializa distâncias e próximos saltos
        if (exists_router[i]) {
            dist_table[i] = INF;
            next_hop[i] = -1; //proximo salto indefinido
        } else {
            dist_table[i] = INF;
            next_hop[i] = -1;
        }
    }
    dist_table[meu_id] = 0;
    next_hop[meu_id] = meu_id;
    pthread_mutex_unlock(&rt_mutex);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <id_roteador>\n", argv[0]);
        exit(1);
    }
    meu_id = atoi(argv[1]);

    read_roteador_config("roteador.config");

    if (!exists_router[meu_id]) {
        port_map[meu_id] = 5000 + meu_id;
        exists_router[meu_id] = 1;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0); // cria socket UDP
    if (sockfd < 0) { 
        perror("Erro socket"); 
        exit(1); 
    }

    read_enlaces_config("enlaces.config");

    init_routing_tables();

    // inicializa distâncias com custos diretos ANTES das threads
    pthread_mutex_lock(&rt_mutex);
    for (int v = 1; v < MAX_ROUTERS; v++) {
        if (vizinho[meu_id][v]) {
            dist_table[v] = custo_enlace[meu_id][v];
            next_hop[v] = v;   // <<< AQUI ESTAVA FALTANDO
        } else {
            dist_table[v] = INF;
            next_hop[v] = -1;
        }
    }
    dist_table[meu_id] = 0;
    next_hop[meu_id] = meu_id;
    pthread_mutex_unlock(&rt_mutex);

    struct sockaddr_in localAddr; // endereço local
    memset(&localAddr, 0, sizeof(localAddr)); // limpa
    localAddr.sin_family = AF_INET; // IPv4
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(port_map[meu_id] > 0 ? port_map[meu_id] : (5000 + meu_id)); // porta

    if (bind(sockfd, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0) { // vincula socket à porta
        perror("Erro bind"); close(sockfd); exit(1);
    }

    fila_init(&filaEntrada);
    fila_init(&filaSaida);

    pthread_t thr_recv, thr_send, thr_pkt, thr_term, thr_bcast; // threads
    pthread_create(&thr_recv, NULL, receiver, NULL);
    pthread_create(&thr_send, NULL, sender, NULL);
    pthread_create(&thr_pkt, NULL, packet_handler, NULL);
    pthread_create(&thr_term, NULL, terminal_thread, NULL);

    pthread_create(&thr_bcast, NULL, envia_dist, NULL);


    printf("[Router %d] iniciado na porta %d\n", meu_id, (int)ntohs(localAddr.sin_port));

    // aguarda threads
    pthread_join(thr_recv, NULL); 
    pthread_join(thr_send, NULL);
    pthread_join(thr_pkt, NULL);
    pthread_join(thr_term, NULL);

    pthread_join(thr_bcast, NULL);

    enviar_vetor_distancias();

    close(sockfd);
    return 0;
}
