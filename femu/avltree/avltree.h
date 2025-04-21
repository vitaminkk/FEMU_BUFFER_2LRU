#ifndef __AVLTREE__
#define __AVLTREE__

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ����սڵ�Ϊ NULL
#define AVL_NULL        (TREE_NODE *)0

// ƽ�����Ӷ���
#define EH_FACTOR   0
#define LH_FACTOR   1
#define RH_FACTOR   -1
#define LEFT_MINUS  0
#define RIGHT_MINUS 1

// �������������꣬������������������
#define ORDER_LIST_WANTED

// ������������ʱ�ķ�����
#define INSERT_PREV 0
#define INSERT_NEXT 1

// AVL���ڵ�ṹ��
typedef struct _AVL_TREE_NODE
{
#ifdef ORDER_LIST_WANTED
    struct _AVL_TREE_NODE* prev;    // ��������ǰһ���ڵ�
    struct _AVL_TREE_NODE* next;    // ����������һ���ڵ�
#endif
    struct _AVL_TREE_NODE* tree_root;   // ���ĸ��ڵ�;���ڵ�
    struct _AVL_TREE_NODE* left_child;  // ����
    struct _AVL_TREE_NODE* right_child; // �Һ���
    int  bf;  /* Balance factor; when the absolute value of the balance factor is greater than or equal to 2, the tree is unbalanced */
} TREE_NODE;

// AVL���ṹ��
typedef struct buffer_info
{
    struct buffer_group* buffer_head;   // LRUͷ�������ʹ��
    struct buffer_group* buffer_tail;   // LRUβ�������δʹ��
    //add
    // ������ͷβָ��
    struct buffer_group* Candidate;
    struct buffer_group* Candidate_tail ;

    // ������ͷβָ��
    struct buffer_group* Hot;
    struct buffer_group* Hot_tail ;
    uint32_t HotList_Size, CandidateList_Size;  
    uint32_t HotList_length , CandidateList_length;// ���б������б��ĳ��ȣ���¼�ڵ����
    //

    TREE_NODE* pTreeHeader;          // ��������Ŀ�� lsn 
    uint32_t count;     // �ڵ�����

#ifdef ORDER_LIST_WANTED
    TREE_NODE* pListHeader;  // ��������ͷ��
    TREE_NODE* pListTail;    // ��������β��
#endif
    int     (*keyCompare)(TREE_NODE*, TREE_NODE*);  // �ȽϽڵ�ĺ���ָ��
    int     (*free)(TREE_NODE*);    // �ͷŽڵ�ĺ���ָ��

    // ͳ����Ϣ
    uint32_t max_secs;
    uint32_t secs_cnt;
    uint32_t read_hit, write_hit;
    uint32_t read_partial_hit, write_partial_hit;
    uint32_t read_miss, write_miss;
    uint32_t more_evict_para;
} tAVLTree;

// ��ȡ���ĸ߶�
int avlTreeHigh(TREE_NODE*);

// �������ƽ����
int avlTreeCheck(tAVLTree*, TREE_NODE*);

// ��������������
void R_Rotate(TREE_NODE**);
void L_Rotate(TREE_NODE**);

// ��ƽ�����ƽ�����
void LeftBalance(TREE_NODE**);
void RightBalance(TREE_NODE**);

// ɾ���ڵ���ƽ�����
int avlDelBalance(tAVLTree*, TREE_NODE*, int);

// ��AVL�����м����ͽ���
void AVL_TREE_LOCK(tAVLTree*, int);
void AVL_TREE_UNLOCK(tAVLTree*);

// �ͷ�AVL���ڵ�
void AVL_TREENODE_FREE(tAVLTree*, TREE_NODE*);

#ifdef ORDER_LIST_WANTED
// ���������Ĳ����ɾ������
int orderListInsert(tAVLTree*, TREE_NODE*, TREE_NODE*, int);
int orderListRemove(tAVLTree*, TREE_NODE*);

// ��ȡAVL���еĵ�һ�������һ������һ����ǰһ���ڵ�
TREE_NODE* avlTreeFirst(tAVLTree*);
TREE_NODE* avlTreeLast(tAVLTree*);
TREE_NODE* avlTreeNext(TREE_NODE* pNode);
TREE_NODE* avlTreePrev(TREE_NODE* pNode);
#endif

// �����ɾ���ڵ��AVL������
int avlTreeInsert(tAVLTree*, TREE_NODE**, TREE_NODE*, int*);
int avlTreeRemove(tAVLTree*, TREE_NODE*);

// ��AVL���в��ҽڵ�
TREE_NODE* avlTreeLookup(tAVLTree*, TREE_NODE*, TREE_NODE*);

// ����AVL��
tAVLTree* avlTreeCreate(int*, int*);

// ����AVL��
int avlTreeDestroy(tAVLTree*);

// ˢ��AVL��
int avlTreeFlush(tAVLTree*);

// ���Ӻ�ɾ���ڵ��AVL������
int avlTreeAdd(tAVLTree*, TREE_NODE*);
int avlTreeDel(tAVLTree*, TREE_NODE*);

// ���ҽڵ�
TREE_NODE* avlTreeFind(tAVLTree*, TREE_NODE*);

// ��ȡAVL���ڵ������
unsigned int avlTreeCount(tAVLTree*);

#endif // __AVLTREE__
