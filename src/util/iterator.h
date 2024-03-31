/*
 * Open Surge Engine
 * iterator.h - general-purpose iterator
 * Copyright 2008-2024 Alexandre Martins <alemartf(at)gmail.com>
 * http://opensurge2d.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ITERATOR_H
#define _ITERATOR_H

/*

Usage example:

int arr[] = { 1, 2, 3, 4, 5 };
size_t n = sizeof(arr) / sizeof(arr[0]);

iterator_t* it = iterator_create_from_array(arr, n, sizeof *arr);
while(iterator_has_next(it)) {
    int* element = iterator_next(it);
    printf("%d ", *element);
}
iterator_destroy(it);

*/

#include <stdbool.h>
#include <stdlib.h>

/* opaque type */
typedef struct iterator_t iterator_t;
typedef void iterator_state_t;

iterator_t* iterator_create(void* ctor_data, iterator_state_t* (*state_ctor)(void*), void (*state_dtor)(iterator_state_t*), void* (*next_fn)(iterator_state_t*), bool (*has_next_fn)(iterator_state_t*)); /* creates a new general-purpose iterator */
iterator_t* iterator_destroy(iterator_t* it); /* destroys an iterator */

bool iterator_has_next(iterator_t* it); /* returns true if the iteration isn't over */
void* iterator_next(iterator_t* it); /* returns a pointer to the next element of the collection and advances the iteration pointer */
bool iterator_foreach(iterator_t* it, void* data, bool (*callback)(void* element, void* data)); /* for each element of the collection, invoke a callback */

/* iterate over C arrays */
typedef iterator_t arrayiterator_t;
arrayiterator_t* iterator_create_from_array(void* array, size_t length, size_t element_size_in_bytes); /* creates a new iterator suitable for iterating over a fixed-size array */

/*
#define ITERATOR_STATE(it) (*((iterator_state_t**)(it)))
*/

#endif