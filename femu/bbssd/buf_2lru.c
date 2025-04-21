#include "../avltree/avltree.h"
#include "./ftl.h"

//#define HC_LOG
//#define ALL_TO_NAND
#define FULL_SO_NO_EVICT 0

bool buf_full_flag = false;
bool hot_full_to_candi_flag = false;
// 尾节点前一个节点的数据域
uint64_t pre_tail_lpn = 0;

//静态函数声明
static void Isolate_NODE(struct ssd* ssd, tAVLTree* buffer, buffer_group* newnode);

//返回1的个数
static inline uint32_t bit_count(uint32_t state) {
    uint32_t res = 0;
    while (state) {
        state &= (state - 1);//消去最低的1
        res++;
    }
    return res;
}

/*
 * Search for buffer node with specific LPN;
 * return NULL (not found) or a ptr.
 */
 // 在 AVL 树中搜索缓冲区中是否存在指定的 lpn，返回对应的 buffer_group 指针
static inline struct buffer_group* buffer_search(tAVLTree* buffer, uint64_t lpn) {
    struct buffer_group key;
    key.group = lpn;
    return (struct buffer_group*)avlTreeFind(buffer, (TREE_NODE*)&key);
}

// 从 SSD 的读缓冲区 (rbuffer) 中删除指定的 lpn 对应的 buffer_group
static void buffer_delete_rnode(struct ssd* ssd, uint64_t lpn) {
    tAVLTree* rbuffer = ssd->rbuffer;

    // 在 AVL 树中搜索 lpn 对应的 buffer_group
    struct buffer_group* rnode = buffer_search(rbuffer, lpn);

    if (rnode != NULL) {
        // 计算 lpn 对应的 buffer_group 中包含的逻辑扇区数量
        uint32_t nsecs = bit_count(rnode->stored);
        rbuffer->secs_cnt -= nsecs;
        ftl_assert(ssd->fp_buf_log);
        //fprintf(ssd->fp_buf_log, "enter buffer_delete_rnode");
        /*从缓存的双链表中断链rnode并维护lru*/
        Isolate_NODE(ssd, rbuffer, rnode);


        // 从 AVL 树中删除
        avlTreeDel(rbuffer, (TREE_NODE*)rnode);

        // 释放节点的内存


        AVL_TREENODE_FREE(rbuffer, (TREE_NODE*)rnode);
        rnode = NULL;

    }
    return;
}


// 从缓冲区中驱逐一页数据到NAND闪存，同时更新相关数据结构
static uint64_t buffer_evict(struct ssd* ssd, tAVLTree* buffer, NvmeRequest* req) {
    uint64_t lat = 0;

    ftl_assert(buffer->Candidate_tail);
    struct buffer_group* temp = buffer->Candidate_tail;
    if (temp == NULL) {
        ftl_err("buffer->Candidate_tail is empty\n");
        return 0;
    }
    pre_tail_lpn = buffer->Candidate_tail->LRU_link_pre->group;
    uint32_t state = temp->stored;
    uint64_t lpn = temp->group;
    bool is_dirty = temp->is_dirty;
    //add
#ifdef HC_LOG
    //fprintf(ssd->fp_buf_log, "buffer_evict ct lpn:%ld,hl:%d,cl:%d,candi_tail_lpn+t:%ld,%ld,%d\n", lpn, buffer->HotList_length, buffer->CandidateList_length, pre_tail_lpn, buffer->Candidate_tail->group, buffer->Candidate_tail->temperature);
#endif
    ftl_assert(temp->temperature==0);
    // 将脏页刷新到 NAND 闪存
    if (is_dirty) {
        // 获取映射表项
        struct ppa ppa = get_maptbl_ent(ssd, lpn);

        // 如果该页已经映射到物理页，则标记为无效，更新映射表和逆向映射表
        if (mapped_ppa(&ppa)) {
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        // 获取新的物理页
        ppa = get_new_page(ssd);

        // 更新映射表
        set_maptbl_ent(ssd, lpn, &ppa);

        // 更新逆向映射表
        set_rmap_ent(ssd, lpn, &ppa);

        // 标记页为有效
        mark_page_valid(ssd, &ppa);

        // 推进写指针
        ssd_advance_write_pointer(ssd);

        // 执行 NAND 闪存写操作，并更新延迟时间
        lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_WRITE, req->stime);

        //add
        ssd->nand_write_page_nb++;
    }
    //if 不是脏页，则删除r/wbuffer的lru尾节点
    // 从 AVL 树和 LRU 列表中删除该节点
    buffer->secs_cnt -= bit_count(state);
    Isolate_NODE(ssd, buffer, temp);
    avlTreeDel(buffer, (TREE_NODE*)temp);

    ///*从cold list中删除尾节点*/

    //
    temp->LRU_link_next = NULL;
    temp->LRU_link_pre = NULL;
    //add
    // 释放节点的内存
    AVL_TREENODE_FREE(buffer, (TREE_NODE*)temp);
    temp = NULL;

    return lat;
}

// 从写缓冲区 (wbuffer) 中读取数据，同时更新缓冲区的统计信息
static inline uint32_t read_from_wbuffer(struct ssd* ssd, uint64_t lpn, uint32_t state) {
    tAVLTree* wbuffer = ssd->wbuffer;
    // 在写缓冲区中搜索 lpn 对应的 buffer_group
    struct buffer_group* wnode = buffer_search(wbuffer, lpn);

    if (wnode != NULL) {
        // 根据写缓冲区中的状态更新输入状态
        state &= (~wnode->stored);

        // 根据更新后的状态进行统计
        if (state == 0) {
            wbuffer->read_hit++;
        }
        else {
            wbuffer->read_partial_hit++;
        }
    }
    else {
        // 如果写缓冲区中不存在该 lpn，则统计为缓冲区未命中
        wbuffer->read_miss++;
    }

    return state;
}

// 从缓冲区中读取数据的函数，同时更新相关数据结构
static uint64_t buffer_read(struct ssd* ssd, NvmeRequest* req, uint64_t lpn, uint32_t state) {
#ifdef ALL_TO_NAND
    uint64_t lat = 0;
    struct ppa ppa = get_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
        lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_READ, req->stime);
    }
    //fprintf(ssd->fp_buf_log, "nand_read lpn:%ld\n", lpn);
#else
    uint64_t lat = DRAM_READ_LATENCY;

    /* 检查写缓冲区。 */
    if (BUFFER_SCHEME == READ_WRITE_PARTITION) {
        // 如果采用读写分离的缓冲区方案，从写缓冲区中读取数据
        state = read_from_wbuffer(ssd, lpn, state);
        if (state == 0) { // 在写缓冲区中完全命中
            /* no add update rbuffer lru。 */
            //add

            //fprintf(ssd->fp_buf_log, "buffer_read lpn:%ld,hl:%d,cl:%d,", lpn, ssd->rbuffer->HotList_length, ssd->rbuffer->CandidateList_length);
            //
        }
        return lat;
    }

    tAVLTree* buffer = ssd->rbuffer;
    struct buffer_group* old_node = buffer_search(buffer, lpn);

    ftl_assert(ssd->fp_buf_log);
#ifdef HC_LOG
    //fprintf(ssd->fp_buf_log, "buffer_read lpn:%ld,hl:%d,cl:%d,", lpn, buffer->HotList_length, buffer->CandidateList_length);
#endif    
    uint32_t nsecs = 0;

    /* 缓冲区未命中 */
    if (old_node == NULL) {
        if (buf_full_flag) {
            //buffer full
            struct ppa ppa = get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
                lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_READ, req->stime);
            }
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "buf_full so nand_read lpn:%ld\n", lpn);
#endif
        }
        else {
            //buffer no full normal io
            struct buffer_group* new_node = NULL;

            // 创建新的缓冲区节点
            new_node = (struct buffer_group*)malloc(sizeof(struct buffer_group));
            if (new_node == NULL) {
                ftl_err("buffer_read malloc fail\n");
                return 0;
            }
            //ftl_assert(new_node);
            new_node->group = lpn;
            new_node->stored = state;
            new_node->is_dirty = false;
            //add
            //init pt
            new_node->LRU_link_next = NULL;
            new_node->LRU_link_pre = NULL;
            new_node->temperature = -1;
            //
            nsecs = bit_count(state);
            //add 
            UPDATE_2LRU(ssd, buffer, new_node);//会更新temperature

            // 插入到 LRU 头部。


            // 插入到 AVL 树中
            avlTreeAdd(buffer, (TREE_NODE*)new_node);

            /* 从 NAND 闪存中读取数据。 */
            struct ppa ppa = get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
                lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_READ, req->stime);
            }
        }
    }
    else {
        /* 移动到 hot-LRU 头部。 */

        UPDATE_2LRU(ssd, buffer, old_node);
        /*auto =if hot list full so move hottail to coldhead*/



        /* 部分命中 or 未命中 */
        if ((state & (~old_node->stored)) != 0) {
            uint32_t new_state = (state | old_node->stored);
            nsecs = bit_count(new_state) - bit_count(old_node->stored);

            old_node->stored = new_state;//更新缓存中的state

            /* 从 NAND 闪存中读取数据。*/
            struct ppa ppa = get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
                lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_READ, req->stime);
            }
        }
    }

    // 更新缓冲区的逻辑扇区统计信息
    buffer->secs_cnt += nsecs;

    // 如果缓冲区的逻辑扇区数量超过了最大限制，进行缓冲区驱逐操作
    if (buffer->secs_cnt > buffer->max_secs) {
        buf_full_flag = true;
    }
    else if (buffer->secs_cnt < buffer->max_secs * 0.95) {
        buf_full_flag = false;
    }
    else {
        // 如果候选列表长度既不大于缓冲区大小，也不小于缓冲区大小的 85%，则不进行任何操作
    }

    if (buf_full_flag) {
        if (FULL_SO_NO_EVICT) {
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "buf_full and no do\n");
#endif
        }
        else
        {
            for (int i = 0; i < buffer->more_evict_para; i++) {
                lat += buffer_evict(ssd, buffer, req);//写缓冲尾节点
            }
        }
    }


#endif

    return lat;
}

/* Buffer API: buffer write */
// 缓冲区写入数据的函数，同时更新相关数据结构
static uint64_t buffer_write(struct ssd* ssd, NvmeRequest* req, uint64_t lpn, uint32_t state) {
#ifdef ALL_TO_NAND
    uint64_t lat = 0;
    struct ppa ppa = get_maptbl_ent(ssd, lpn);

    // 如果该页已经映射到物理页，则标记为无效，更新映射表和逆向映射表
    if (mapped_ppa(&ppa)) {
        mark_page_invalid(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
    }

    // 获取新的物理页
    ppa = get_new_page(ssd);

    // 更新映射表
    set_maptbl_ent(ssd, lpn, &ppa);

    // 更新逆向映射表
    set_rmap_ent(ssd, lpn, &ppa);

    // 标记页为有效
    mark_page_valid(ssd, &ppa);

    // 推进写指针
    ssd_advance_write_pointer(ssd);

    // 执行 NAND 闪存写操作，并更新延迟时间
    lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_WRITE, req->stime);
    //add
    ssd->nand_write_page_nb++;
#else
    uint64_t lat = DRAM_WRITE_LATENCY;
    tAVLTree* buffer = ssd->wbuffer;
    struct buffer_group* old_node = buffer_search(buffer, lpn);

#ifdef HC_LOG
    //fprintf(ssd->fp_buf_log, "buffer_write lpn:%ld,hl:%d,cl:%d,", lpn, buffer->HotList_length, buffer->CandidateList_length);
#endif
    //
    uint32_t nsecs = 0;

    // 读写buffer一致性的简单处理：在写的时候直接将整个node在读buffer中删除
    if (BUFFER_SCHEME == READ_WRITE_PARTITION) {
        buffer_delete_rnode(ssd, lpn);

    }

    /* 缓冲区未命中 */
    if (old_node == NULL) {
        if (buf_full_flag) {
            //full so nand write
            struct ppa ppa = get_maptbl_ent(ssd, lpn);
            // 如果该页已经映射到物理页，则标记为无效，更新映射表和逆向映射表
            if (mapped_ppa(&ppa)) {
                mark_page_invalid(ssd, &ppa);
                set_rmap_ent(ssd, INVALID_LPN, &ppa);
            }

            // 获取新的物理页
            ppa = get_new_page(ssd);

            // 更新映射表
            set_maptbl_ent(ssd, lpn, &ppa);

            // 更新逆向映射表
            set_rmap_ent(ssd, lpn, &ppa);

            // 标记页为有效
            mark_page_valid(ssd, &ppa);

            // 推进写指针
            ssd_advance_write_pointer(ssd);

            // 执行 NAND 闪存写操作，并更新延迟时间
            lat += ssd_nand_rw(ssd, &ppa, USER_IO, NAND_WRITE, req->stime);
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "buf_full so nand_write lpn:%ld\n", lpn);
#endif
            //add
            ssd->nand_write_page_nb++;

        }
        else {
            //buffer no full normal dram io
        /* 创建新的节点 */
            struct buffer_group* new_node = NULL;
            new_node = (struct buffer_group*)malloc(sizeof(struct buffer_group));
            if (new_node == NULL) {
                ftl_err("buffer_write malloc fail\n");
                return 0;
            }
            ftl_assert(new_node);
            new_node->group = lpn;
            new_node->stored = state;
            new_node->is_dirty = true;
            //add
            //init pt
            new_node->LRU_link_next = NULL;
            new_node->LRU_link_pre = NULL;
            new_node->temperature = -1;
            nsecs = bit_count(state);
            /* 插入到cold-LRU 头部。 */
            UPDATE_2LRU(ssd, buffer, new_node);

            // 插入到 AVL 树中
            avlTreeAdd(buffer, (TREE_NODE*)new_node);
        }
    }
    else {
        old_node->is_dirty = true;
        //old_node->temperature = 1;
        /* 部分命中 */
        if ((state & (~old_node->stored)) != 0) {
            uint32_t new_state = state | old_node->stored;
            nsecs = bit_count(new_state) - bit_count(old_node->stored);//多写的扇区数 1001-0001 》=1
            old_node->stored = new_state;
        }

        /* 移动到 hot-LRU 头部。 */
        UPDATE_2LRU(ssd, buffer, old_node);

    }

    // 更新缓冲区的逻辑扇区统计信息
    buffer->secs_cnt += nsecs;

    // 如果缓冲区的逻辑扇区数量超过了最大限制，进行缓冲区驱逐操作
    if (buffer->secs_cnt > buffer->max_secs) {
        buf_full_flag = true;
    }
    else if (buffer->secs_cnt < buffer->max_secs * 0.95) {
        buf_full_flag = false;
    }
    else {
        // 如果候选列表长度既不大于缓冲区大小，也不小于缓冲区大小的 85%，则不进行任何操作
    }

    if (buf_full_flag) {
        if (FULL_SO_NO_EVICT) {
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "buf_full and no do\n");
#endif
        }
        else
        {
            for (int i = 0; i < buffer->more_evict_para; i++) {
                lat += buffer_evict(ssd, buffer, req);//写缓冲尾节点
            }
        }
    }


#endif
    return lat;
}


/* AvlTree: key compare function */
static int keyCompareFunc(TREE_NODE* p1, TREE_NODE* p2)
{
    struct buffer_group* T1 = NULL, * T2 = NULL;

    T1 = (struct buffer_group*)p1;
    T2 = (struct buffer_group*)p2;

    // 比较两个 buffer_group 节点的逻辑扇区号，返回结果
    if (T1->group < T2->group) return 1;
    if (T1->group > T2->group) return -1;

    return 0;
}

/* AvlTree: free node function */
static int freeFunc(TREE_NODE* pNode)
{
    if (pNode != NULL)
    {
        free((void*)pNode);
    }

    pNode = NULL;
    return 1;
}

/* 注册缓冲区读写函数 */
static void buffer_register(struct ssd* ssd) {
    switch (BUFFER_SCHEME) {
    case READ_WRITE_HYBRID:
        // 读写混合缓冲区方案，设置读写函数指针
        ssd->buffer_read = &buffer_read;
        ssd->buffer_write = &buffer_write;
        break;

    case READ_WRITE_PARTITION:
        // 读写分离缓冲区方案
        ssd->buffer_read = &buffer_read;
        ssd->buffer_write = &buffer_write;
        break;

    default:
        // 错误处理
        printf("ERROR[%s]: register failed.\n", __FUNCTION__);
        fflush(stdout);
        assert(0);
        break;
    }
}


/* SSD 初始化缓冲区 */
void ssd_init_buffer(struct ssd* ssd, uint32_t dramsz_mb) {
    uint32_t secs_cnt = dramsz_mb * 1024 * 1024 / SEC_SIZE;//BYTE/BYTE
    ssd->wbuffer = avlTreeCreate((void*)keyCompareFunc, (void*)freeFunc);
    switch (BUFFER_SCHEME)
    {
        /* 读写混合缓冲区，写缓冲区和读缓冲区实际上是同一个缓冲区 */
    case READ_WRITE_HYBRID:
        ssd->wbuffer->max_secs = secs_cnt;
        ssd->rbuffer = ssd->wbuffer;
        ssd->wbuffer->more_evict_para = 1;
        initialize_lists(ssd, ssd->wbuffer);

        break;

        /* 读写分离缓冲区 */
    case READ_WRITE_PARTITION:
        ssd->rbuffer = avlTreeCreate((void*)keyCompareFunc, (void*)freeFunc);
        // 写缓冲区和读缓冲区的逻辑扇区数量分别一半
        ssd->wbuffer->max_secs = secs_cnt / 2;
        ssd->rbuffer->max_secs = secs_cnt - ssd->wbuffer->max_secs;
        //ssd->fp_buf_log = fopen("/home/zxk/Desktop/data/buffer-2lru/buf_log.txt", "a");
        //ssd->fp_buf_log = fopen("/home/zxk/Desktop/data/buffer-2lru/buf_log.txt", "a");
        initialize_lists(ssd, ssd->wbuffer);
        initialize_lists(ssd, ssd->rbuffer);


        break;

    default:
        // 错误处理
        ftl_assert(0);
        break;
    }
    // 注册缓冲区读写函数
    buffer_register(ssd);
    //add

}

//only hybrid
void initialize_lists(struct ssd* ssd, tAVLTree* buffer) {
    buffer->HotList_Size = buffer->max_secs / 8 / 16; //1:1=h:c
    buffer->CandidateList_Size = buffer->max_secs / 8 - buffer->HotList_Size;
    buffer->HotList_length = 0;
    buffer->CandidateList_length = 0;
    buffer->Hot = NULL;
    buffer->Candidate = NULL;
#ifdef HC_LOG

    //fprintf(ssd->fp_buf_log, "enter initialize_lists,HS:%d,CS:%d\n", buffer->HotList_Size, buffer->CandidateList_Size);

#endif // HC_LOG


}

void UPDATE_2LRU(struct ssd* ssd, tAVLTree* buffer, buffer_group* newnode) {

    if ((newnode->temperature == 1)) {
        Broken_chain(ssd, buffer, &buffer->Hot, newnode);//graud lru mes
        buffer->Hot = insert_head_List(ssd, buffer, buffer->Hot, newnode);


#ifdef HC_LOG

        //fprintf(ssd->fp_buf_log, "enter UPDATE_2LRU: hot to hot\n");

#endif // HC_LOG
    }
    else if ((newnode->temperature == 0)) {
        if (buffer->HotList_length >= buffer->HotList_Size) {
            ftl_assert(buffer->Hot_tail);
            buffer_group* temp = buffer->Hot_tail;
            if(temp->temperature!=0)
            {}
            else{
                temp = buffer->Hot;
                ftl_assert(temp);
                for(int j=0;j<=buffer->HotList_length;j++)
                {
                /*if(temp->LRU_link_next->temperature==0)
                {
                    break;
                }
                else{
                    temp = temp->LRU_link_next;
                }*/
                
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "hot list:%ld t:%d\n", temp->group, temp->temperature);
#endif 
                temp = temp->LRU_link_next;
                ftl_assert(temp);
                }
                ftl_err("memory err\n");
                ftl_assert(0);
                
            }
            pre_tail_lpn = temp->LRU_link_pre->group;
            if (temp->group != buffer->Hot_tail->group) ftl_err("temp->group!= buffer->Hot_tail->group\n");
#ifdef HC_LOG
            //fprintf(ssd->fp_buf_log, "hot tail lpn:%ld %ld %ld,t:%d\n", pre_tail_lpn, temp->group, buffer->Hot_tail->group, temp->temperature);
#endif 
            Broken_chain(ssd, buffer, &buffer->Hot, temp);
            buffer->HotList_length--;
            temp->temperature = 0;//low temperature
            buffer->Candidate = insert_head_List(ssd, buffer, buffer->Candidate, temp);
            buffer->CandidateList_length++;
            hot_full_to_candi_flag = true;

        }
        Broken_chain(ssd, buffer, &buffer->Candidate, newnode);
        buffer->CandidateList_length--;
        newnode->temperature = 1;//up temperature  only time
        buffer->Hot = insert_head_List(ssd, buffer, buffer->Hot, newnode);
        buffer->HotList_length++;



#ifdef HC_LOG

        /*if (hot_full_to_candi_flag == false)
            fprintf(ssd->fp_buf_log, "enter UPDATE_2LRU: candi to hot\n");
        else
            fprintf(ssd->fp_buf_log, "enter UPDATE_2LRU: alternate hot tail\n");*/

#endif // HC_LOG
    }
    else {
        newnode->temperature = 0;//new to candi -1 to 0 
        buffer->Candidate = insert_head_List(ssd, buffer, buffer->Candidate, newnode);//ͷ������
        buffer->CandidateList_length++;

#ifdef HC_LOG

        //fprintf(ssd->fp_buf_log, "enter UPDATE_2LRU: nand or newreq to candi\n");

#endif // HC_LOG
    }
}

static void Isolate_NODE(struct ssd* ssd, tAVLTree* buffer, buffer_group* newnode) {
    if (newnode == NULL) {
        ftl_err("Isolate_NODE fail, node is NULL\n");
        ftl_assert(0);
        return;
    }

    if (newnode->temperature == 1) {
        Broken_chain(ssd, buffer, &buffer->Hot, newnode);
        buffer->HotList_length--;
    }
    else if (newnode->temperature == 0) {
        Broken_chain(ssd, buffer, &buffer->Candidate, newnode);
        buffer->CandidateList_length--;
    }
    else {
        ftl_err("Isolate_NODE fail, unexpected temperature\n");
        ftl_assert(0);
    }

}

buffer_group* insert_head_List(struct ssd* ssd, tAVLTree* buffer, buffer_group* head, buffer_group* newnode) {
    buffer_group* temp = newnode;
    if (head == NULL) {
        if (newnode->temperature == 0){
            buffer->Candidate_tail = temp;
            temp->LRU_link_next=NULL;
        }

        else if (newnode->temperature == 1){
            buffer->Hot_tail = temp;
            temp->LRU_link_next=NULL;
        }

        else
        {
            ftl_err("insert_head_List err temperature: %d\n", newnode->temperature);
            ftl_assert(0);
        }

        return temp;  
    }
    temp->LRU_link_next = head;
    head->LRU_link_pre = temp;
    return temp;
}


void Broken_chain(struct ssd* ssd, tAVLTree* buffer, buffer_group** head, buffer_group* target) {

    if (target->LRU_link_pre == NULL && target->LRU_link_next == NULL) {
        *head = NULL;
    }
    else if (target->LRU_link_pre == NULL) {//维护 头结点
        target->LRU_link_next->LRU_link_pre = NULL;
        *head = target->LRU_link_next;
    }
    else if (target->LRU_link_next == NULL) {//维护尾节点
        target->LRU_link_pre->LRU_link_next = NULL;
        if (target->temperature == 0)
            buffer->Candidate_tail = target->LRU_link_pre;
        else if(target->temperature == 1)
        {
            buffer->Hot_tail = target->LRU_link_pre;
        }
        else
        {}
    }
    else {
        target->LRU_link_pre->LRU_link_next = target->LRU_link_next;
        target->LRU_link_next->LRU_link_pre = target->LRU_link_pre;
    }
    //really broken
    target->LRU_link_next=NULL;
    target->LRU_link_pre=NULL;
    //free(target);  
}

__attribute__((unused)) static void xlist_delete_node(struct ssd* ssd, tAVLTree* buffer, buffer_group* newnode) {

// 计算 lpn 对应的 buffer_group 中包含的逻辑扇区数量
        uint32_t nsecs = bit_count(newnode->stored);
        buffer->secs_cnt -= nsecs;
        //fprintf(ssd->fp_buf_log, "enter xlist_delete_node lpn:%ld\n",newnode->group);
        /*从缓存的双链表中断链rnode并维护lru*/
        Isolate_NODE(ssd, buffer, newnode);
        // 从 AVL 树中删除
        avlTreeDel(buffer, (TREE_NODE*)newnode);
        // 释放节点的内存
        AVL_TREENODE_FREE(buffer, (TREE_NODE*)newnode);
        newnode = NULL;
    return;
}

__attribute__((unused)) void print_xList(struct ssd* ssd, tAVLTree* buffer, buffer_group* list) {
    if (list == NULL) printf("xList Empty\n");
    else {
        printf("xList : ");
        while (list) {
            printf("%ld", list->group);
            list = list->LRU_link_next;
        }
        printf("\n");
    }
}

