/**
 * @file list.c
 * @brief Intrusive list utility implementation.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "../ahura.h"

#if (OS_CONFIG_LIST_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize list container.
 *
 * @param[in,out] list  List object.
 * @return None.
 */
void os_list_init(os_list_t *list)
{
    if (list == NULL)
    {
        return;
    }

    list->head = NULL;
    list->tail = NULL;
}

/******************************************************************************************************/
/**
 * @brief Check whether list is empty.
 *
 * @param[in] list  List object.
 * @return bool     True when list has no nodes.
 */
bool os_list_is_empty(const os_list_t *list)
{
    if (list == NULL)
    {
        return true;
    }

    return (list->head == NULL);
}

/******************************************************************************************************/
/**
 * @brief Push node at list tail.
 *
 * @param[in,out] list  List object.
 * @param[in,out] node  Node to append.
 * @return None.
 */
void os_list_push_back(os_list_t *list, os_list_node_t *node)
{
    if ((list == NULL) || (node == NULL))
    {
        return;
    }

    node->next = NULL;
    node->prev = list->tail;

    if (list->tail != NULL)
    {
        list->tail->next = node;
    }
    else
    {
        list->head = node;
    }

    list->tail = node;
}

/******************************************************************************************************/
/**
 * @brief Pop one node from list head.
 *
 * @param[in,out] list  List object.
 * @return os_list_node_t* Head node or NULL when empty.
 */
os_list_node_t *os_list_pop_front(os_list_t *list)
{
    os_list_node_t *node = NULL;

    if ((list == NULL) || (list->head == NULL))
    {
        return NULL;
    }

    node       = list->head;
    list->head = node->next;

    if (list->head != NULL)
    {
        list->head->prev = NULL;
    }
    else
    {
        list->tail = NULL;
    }

    node->next = NULL;
    node->prev = NULL;

    return node;
}

#endif /* OS_CONFIG_LIST_ENABLE */
