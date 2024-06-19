/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2024

#include "internals.h"

#ifdef __SANITIZE_THREAD__
/* LY: avoid tsan-trap by txn, mm_last_pg and geo.first_unallocated */
__attribute__((__no_sanitize_thread__, __noinline__))
#endif
int mdbx_txn_straggler(const MDBX_txn *txn, int *percent)
{
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return (rc > 0) ? -rc : rc;

  MDBX_env *env = txn->env;
  if (unlikely((txn->flags & MDBX_TXN_RDONLY) == 0)) {
    if (percent)
      *percent = (int)((txn->geo.first_unallocated * UINT64_C(100) +
                        txn->geo.end_pgno / 2) /
                       txn->geo.end_pgno);
    return 0;
  }

  txnid_t lag;
  troika_t troika = meta_tap(env);
  do {
    const meta_ptr_t head = meta_recent(env, &troika);
    if (percent) {
      const pgno_t maxpg = head.ptr_v->geometry.now;
      *percent = (int)((head.ptr_v->geometry.first_unallocated * UINT64_C(100) +
                        maxpg / 2) /
                       maxpg);
    }
    lag = (head.txnid - txn->txnid) / xMDBX_TXNID_STEP;
  } while (unlikely(meta_should_retry(env, &troika)));

  return (lag > INT_MAX) ? INT_MAX : (int)lag;
}

__cold int mdbx_dbi_dupsort_depthmask(const MDBX_txn *txn, MDBX_dbi dbi,
                                      uint32_t *mask) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!mask))
    return MDBX_EINVAL;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if ((cx.outer.tree->flags & MDBX_DUPSORT) == 0)
    return MDBX_RESULT_TRUE;

  MDBX_val key, data;
  rc = outer_first(&cx.outer, &key, &data);
  *mask = 0;
  while (rc == MDBX_SUCCESS) {
    const node_t *node =
        page_node(cx.outer.pg[cx.outer.top], cx.outer.ki[cx.outer.top]);
    const tree_t *db = node_data(node);
    const unsigned flags = node_flags(node);
    switch (flags) {
    case N_BIGDATA:
    case 0:
      /* single-value entry, deep = 0 */
      *mask |= 1 << 0;
      break;
    case N_DUPDATA:
      /* single sub-page, deep = 1 */
      *mask |= 1 << 1;
      break;
    case N_DUPDATA | N_SUBDATA:
      /* sub-tree */
      *mask |= 1 << UNALIGNED_PEEK_16(db, tree_t, height);
      break;
    default:
      ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED,
            "invalid node-size", flags);
      return MDBX_CORRUPTED;
    }
    rc = outer_next(&cx.outer, &key, &data, MDBX_NEXT_NODUP);
  }

  return (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
}

int mdbx_canary_get(const MDBX_txn *txn, MDBX_canary *canary) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(canary == nullptr))
    return MDBX_EINVAL;

  *canary = txn->canary;
  return MDBX_SUCCESS;
}

int mdbx_get(const MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
             MDBX_val *data) {
  DKBUF_DEBUG;
  DEBUG("===> get db %u key [%s]", dbi, DKEY_DEBUG(key));

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return cursor_seek(&cx.outer, (MDBX_val *)key, data, MDBX_SET).err;
}

int mdbx_get_equal_or_great(const MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key,
                            MDBX_val *data) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(txn->flags & MDBX_TXN_BLOCKED))
    return MDBX_BAD_TXN;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  return cursor_ops(&cx.outer, key, data, MDBX_SET_LOWERBOUND);
}

int mdbx_get_ex(const MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *key,
                MDBX_val *data, size_t *values_count) {
  DKBUF_DEBUG;
  DEBUG("===> get db %u key [%s]", dbi, DKEY_DEBUG(key));

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  rc = cursor_seek(&cx.outer, key, data, MDBX_SET_KEY).err;
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (values_count)
      *values_count = 0;
    return rc;
  }

  if (values_count) {
    *values_count = 1;
    if (inner_pointed(&cx.outer))
      *values_count =
          (sizeof(*values_count) >= sizeof(cx.inner.nested_tree.items) ||
           cx.inner.nested_tree.items <= PTRDIFF_MAX)
              ? (size_t)cx.inner.nested_tree.items
              : PTRDIFF_MAX;
  }
  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

int mdbx_canary_put(MDBX_txn *txn, const MDBX_canary *canary) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (likely(canary)) {
    if (txn->canary.x == canary->x && txn->canary.y == canary->y &&
        txn->canary.z == canary->z)
      return MDBX_SUCCESS;
    txn->canary.x = canary->x;
    txn->canary.y = canary->y;
    txn->canary.z = canary->z;
  }
  txn->canary.v = txn->txnid;
  txn->flags |= MDBX_TXN_DIRTY;

  return MDBX_SUCCESS;
}

/* Функция сообщает находится ли указанный адрес в "грязной" странице у
 * заданной пишущей транзакции. В конечном счете это позволяет избавиться от
 * лишнего копирования данных из НЕ-грязных страниц.
 *
 * "Грязные" страницы - это те, которые уже были изменены в ходе пишущей
 * транзакции. Соответственно, какие-либо дальнейшие изменения могут привести
 * к перезаписи таких страниц. Поэтому все функции, выполняющие изменения, в
 * качестве аргументов НЕ должны получать указатели на данные в таких
 * страницах. В свою очередь "НЕ грязные" страницы перед модификацией будут
 * скопированы.
 *
 * Другими словами, данные из "грязных" страниц должны быть либо скопированы
 * перед передачей в качестве аргументов для дальнейших модификаций, либо
 * отвергнуты на стадии проверки корректности аргументов.
 *
 * Таким образом, функция позволяет как избавится от лишнего копирования,
 * так и выполнить более полную проверку аргументов.
 *
 * ВАЖНО: Передаваемый указатель должен указывать на начало данных. Только
 * так гарантируется что актуальный заголовок страницы будет физически
 * расположен в той-же странице памяти, в том числе для многостраничных
 * P_LARGE страниц с длинными данными. */
int mdbx_is_dirty(const MDBX_txn *txn, const void *ptr) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const MDBX_env *env = txn->env;
  const ptrdiff_t offset = ptr_dist(ptr, env->dxb_mmap.base);
  if (offset >= 0) {
    const pgno_t pgno = bytes2pgno(env, offset);
    if (likely(pgno < txn->geo.first_unallocated)) {
      const page_t *page = pgno2page(env, pgno);
      if (unlikely(page->pgno != pgno || (page->flags & P_ILL_BITS) != 0)) {
        /* The ptr pointed into middle of a large page,
         * not to the beginning of a data. */
        return MDBX_EINVAL;
      }
      return ((txn->flags & MDBX_TXN_RDONLY) || !is_modifable(txn, page))
                 ? MDBX_RESULT_FALSE
                 : MDBX_RESULT_TRUE;
    }
    if ((size_t)offset < env->dxb_mmap.limit) {
      /* Указатель адресует что-то в пределах mmap, но за границей
       * распределенных страниц. Такое может случится если mdbx_is_dirty()
       * вызывается после операции, в ходе которой грязная страница была
       * возвращена в нераспределенное пространство. */
      return (txn->flags & MDBX_TXN_RDONLY) ? MDBX_EINVAL : MDBX_RESULT_TRUE;
    }
  }

  /* Страница вне используемого mmap-диапазона, т.е. либо в функцию был
   * передан некорректный адрес, либо адрес в теневой странице, которая была
   * выделена посредством malloc().
   *
   * Для режима MDBX_WRITE_MAP режима страница однозначно "не грязная",
   * а для режимов без MDBX_WRITE_MAP однозначно "не чистая". */
  return (txn->flags & (MDBX_WRITEMAP | MDBX_TXN_RDONLY)) ? MDBX_EINVAL
                                                          : MDBX_RESULT_TRUE;
}

int mdbx_del(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
             const MDBX_val *data) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key))
    return MDBX_EINVAL;

  if (unlikely(dbi <= FREE_DBI))
    return MDBX_BAD_DBI;

  if (unlikely(txn->flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_val proxy;
  MDBX_cursor_op op = MDBX_SET;
  unsigned flags = MDBX_ALLDUPS;
  if (data) {
    proxy = *data;
    data = &proxy;
    op = MDBX_GET_BOTH;
    flags = 0;
  }
  rc = cursor_seek(&cx.outer, (MDBX_val *)key, (MDBX_val *)data, op).err;
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  cx.outer.next = txn->cursors[dbi];
  txn->cursors[dbi] = &cx.outer;
  rc = cursor_del(&cx.outer, flags);
  txn->cursors[dbi] = cx.outer.next;
  return rc;
}

int mdbx_put(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key, MDBX_val *data,
             MDBX_put_flags_t flags) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !data))
    return MDBX_EINVAL;

  if (unlikely(dbi <= FREE_DBI))
    return MDBX_BAD_DBI;

  if (unlikely(flags & ~(MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_ALLDUPS |
                         MDBX_ALLDUPS | MDBX_RESERVE | MDBX_APPEND |
                         MDBX_APPENDDUP | MDBX_CURRENT | MDBX_MULTIPLE)))
    return MDBX_EINVAL;

  if (unlikely(txn->flags & (MDBX_TXN_RDONLY | MDBX_TXN_BLOCKED)))
    return (txn->flags & MDBX_TXN_RDONLY) ? MDBX_EACCESS : MDBX_BAD_TXN;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  cx.outer.next = txn->cursors[dbi];
  txn->cursors[dbi] = &cx.outer;

  /* LY: support for update (explicit overwrite) */
  if (flags & MDBX_CURRENT) {
    rc = cursor_seek(&cx.outer, (MDBX_val *)key, nullptr, MDBX_SET).err;
    if (likely(rc == MDBX_SUCCESS) && (txn->dbs[dbi].flags & MDBX_DUPSORT) &&
        (flags & MDBX_ALLDUPS) == 0) {
      /* LY: allows update (explicit overwrite) only for unique keys */
      node_t *node =
          page_node(cx.outer.pg[cx.outer.top], cx.outer.ki[cx.outer.top]);
      if (node_flags(node) & N_DUPDATA) {
        tASSERT(txn, inner_pointed(&cx.outer) &&
                         cx.outer.subcur->nested_tree.items > 1);
        rc = MDBX_EMULTIVAL;
        if ((flags & MDBX_NOOVERWRITE) == 0) {
          flags -= MDBX_CURRENT;
          rc = cursor_del(&cx.outer, MDBX_ALLDUPS);
        }
      }
    }
  }

  if (likely(rc == MDBX_SUCCESS))
    rc = cursor_put_checklen(&cx.outer, key, data, flags);
  txn->cursors[dbi] = cx.outer.next;

  return rc;
}

//------------------------------------------------------------------------------

/* Позволяет обновить или удалить существующую запись с получением
 * в old_data предыдущего значения данных. При этом если new_data равен
 * нулю, то выполняется удаление, иначе обновление/вставка.
 *
 * Текущее значение может находиться в уже измененной (грязной) странице.
 * В этом случае страница будет перезаписана при обновлении, а само старое
 * значение утрачено. Поэтому исходно в old_data должен быть передан
 * дополнительный буфер для копирования старого значения.
 * Если переданный буфер слишком мал, то функция вернет -1, установив
 * old_data->iov_len в соответствующее значение.
 *
 * Для не-уникальных ключей также возможен второй сценарий использования,
 * когда посредством old_data из записей с одинаковым ключом для
 * удаления/обновления выбирается конкретная. Для выбора этого сценария
 * во flags следует одновременно указать MDBX_CURRENT и MDBX_NOOVERWRITE.
 * Именно эта комбинация выбрана, так как она лишена смысла, и этим позволяет
 * идентифицировать запрос такого сценария.
 *
 * Функция может быть замещена соответствующими операциями с курсорами
 * после двух доработок (TODO):
 *  - внешняя аллокация курсоров, в том числе на стеке (без malloc).
 *  - получения dirty-статуса страницы по адресу (знать о MUTABLE/WRITEABLE).
 */

int mdbx_replace_ex(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
                    MDBX_val *new_data, MDBX_val *old_data,
                    MDBX_put_flags_t flags, MDBX_preserve_func preserver,
                    void *preserver_context) {
  int rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (unlikely(!key || !old_data || old_data == new_data))
    return MDBX_EINVAL;

  if (unlikely(old_data->iov_base == nullptr && old_data->iov_len))
    return MDBX_EINVAL;

  if (unlikely(new_data == nullptr &&
               (flags & (MDBX_CURRENT | MDBX_RESERVE)) != MDBX_CURRENT))
    return MDBX_EINVAL;

  if (unlikely(dbi <= FREE_DBI))
    return MDBX_BAD_DBI;

  if (unlikely(flags &
               ~(MDBX_NOOVERWRITE | MDBX_NODUPDATA | MDBX_ALLDUPS |
                 MDBX_RESERVE | MDBX_APPEND | MDBX_APPENDDUP | MDBX_CURRENT)))
    return MDBX_EINVAL;

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  cx.outer.next = txn->cursors[dbi];
  txn->cursors[dbi] = &cx.outer;

  MDBX_val present_key = *key;
  if (F_ISSET(flags, MDBX_CURRENT | MDBX_NOOVERWRITE)) {
    /* в old_data значение для выбора конкретного дубликата */
    if (unlikely(!(txn->dbs[dbi].flags & MDBX_DUPSORT))) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    /* убираем лишний бит, он был признаком запрошенного режима */
    flags -= MDBX_NOOVERWRITE;

    rc = cursor_seek(&cx.outer, &present_key, old_data, MDBX_GET_BOTH).err;
    if (rc != MDBX_SUCCESS)
      goto bailout;
  } else {
    /* в old_data буфер для сохранения предыдущего значения */
    if (unlikely(new_data && old_data->iov_base == new_data->iov_base))
      return MDBX_EINVAL;
    MDBX_val present_data;
    rc = cursor_seek(&cx.outer, &present_key, &present_data, MDBX_SET_KEY).err;
    if (unlikely(rc != MDBX_SUCCESS)) {
      old_data->iov_base = nullptr;
      old_data->iov_len = 0;
      if (rc != MDBX_NOTFOUND || (flags & MDBX_CURRENT))
        goto bailout;
    } else if (flags & MDBX_NOOVERWRITE) {
      rc = MDBX_KEYEXIST;
      *old_data = present_data;
      goto bailout;
    } else {
      page_t *page = cx.outer.pg[cx.outer.top];
      if (txn->dbs[dbi].flags & MDBX_DUPSORT) {
        if (flags & MDBX_CURRENT) {
          /* disallow update/delete for multi-values */
          node_t *node = page_node(page, cx.outer.ki[cx.outer.top]);
          if (node_flags(node) & N_DUPDATA) {
            tASSERT(txn, inner_pointed(&cx.outer) &&
                             cx.outer.subcur->nested_tree.items > 1);
            if (cx.outer.subcur->nested_tree.items > 1) {
              rc = MDBX_EMULTIVAL;
              goto bailout;
            }
          }
          /* В LMDB флажок MDBX_CURRENT здесь приведет
           * к замене данных без учета MDBX_DUPSORT сортировки,
           * но здесь это в любом случае допустимо, так как мы
           * проверили что для ключа есть только одно значение. */
        }
      }

      if (is_modifable(txn, page)) {
        if (new_data && cmp_lenfast(&present_data, new_data) == 0) {
          /* если данные совпадают, то ничего делать не надо */
          *old_data = *new_data;
          goto bailout;
        }
        rc = preserver ? preserver(preserver_context, old_data,
                                   present_data.iov_base, present_data.iov_len)
                       : MDBX_SUCCESS;
        if (unlikely(rc != MDBX_SUCCESS))
          goto bailout;
      } else {
        *old_data = present_data;
      }
      flags |= MDBX_CURRENT;
    }
  }

  if (likely(new_data))
    rc = cursor_put_checklen(&cx.outer, key, new_data, flags);
  else
    rc = cursor_del(&cx.outer, flags & MDBX_ALLDUPS);

bailout:
  txn->cursors[dbi] = cx.outer.next;
  return rc;
}

static int default_value_preserver(void *context, MDBX_val *target,
                                   const void *src, size_t bytes) {
  (void)context;
  if (unlikely(target->iov_len < bytes)) {
    target->iov_base = nullptr;
    target->iov_len = bytes;
    return MDBX_RESULT_TRUE;
  }
  memcpy(target->iov_base, src, target->iov_len = bytes);
  return MDBX_SUCCESS;
}

int mdbx_replace(MDBX_txn *txn, MDBX_dbi dbi, const MDBX_val *key,
                 MDBX_val *new_data, MDBX_val *old_data,
                 MDBX_put_flags_t flags) {
  return mdbx_replace_ex(txn, dbi, key, new_data, old_data, flags,
                         default_value_preserver, nullptr);
}
