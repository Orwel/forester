#include "../sl.h"
#include <stdlib.h>

typedef struct sll          sll_t;
typedef struct sll_item     sll_item_t;

struct sll {
    sll_item_t      *head;
    sll_item_t      **last;
};

struct sll_item {
    sll_item_t      *next;
    sll_t           *list;
};

#define ALLOC(type) \
    ((type *) malloc(sizeof(type)))

static void append_item(sll_t *list)
{
    sll_item_t **dst = list->last;

    *dst = ALLOC(sll_item_t);
    if (!*dst)
        abort();

    (*dst)->next = NULL;
    (*dst)->list = list;

    list->last = &(*dst)->next;
}

int main()
{
    ___sl_plot("test38-00");
    sll_t list;
    list.head = NULL;
    list.last = &list.head;
    ___sl_plot("test38-01");    // print empty list state

    for(int i=0; i<100; i++) 
        append_item(&list);
    
    ___sl_plot("test38-02");
    {
        // delete first
        sll_item_t *next = list.head->next;
        free(list.head);
        list.head = next;
    }

    ___sl_plot("test38-03");

    while (list.head) {
        sll_item_t *next = list.head->next;
        if (!next)
            ___sl_plot("test38-04");    // print if single item
        free(list.head);
        list.head = next;
    }

    ___sl_plot("test38-05");            // print final state
    return 0;
}
