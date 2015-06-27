
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#include "levenshtein.h"

/* TODO: write an explanation / proof of the algorithm */

struct range {
	unsigned char ch;       /* the character in a currently checked  */
	const unsigned char *a; /* query substring to look for           */
	unsigned length_a;      /* number of chars left to check in a    */
	const unsigned char *b; /* beggining of the current range        */
	unsigned length_b;      /* length of the current range           */
	unsigned *row;          /* current row in the levenshtein matrix */
	unsigned *row_prev;     /* previous row                          */
	unsigned max_distance;  /* current value of the treshold         */
	int a_index;            /* the 1 based index of ch in a          */

	/* the edges of the currently observed subrange */
	unsigned first_match;   /* the first element < max_distance      */
	unsigned last_match;    /* the last element < max_distance       */
};

/* TODO: eliminate these globals
 *  to make this thread-safe,
 *  and accept arbitrarily long strings
 */
static unsigned storage0[LEVENSHTEIN_MAX_LENGTH + 2];
static unsigned storage1[LEVENSHTEIN_MAX_LENGTH + 2];

static void setup_initial_range(struct range *initial,
	const char *a, unsigned length_a,
	const char *b, unsigned length_b,
	unsigned max_distance);
static unsigned process_range(struct range *);

unsigned levenshtein_substring_match(const char *a, unsigned length_a,
	const char *b, unsigned length_b,
	unsigned max_distance)
{
	struct range initial;
	unsigned result;

	if (length_a > LEVENSHTEIN_MAX_LENGTH) {
		length_a = LEVENSHTEIN_MAX_LENGTH;
	}
	if (length_b > LEVENSHTEIN_MAX_LENGTH) {
		length_b = LEVENSHTEIN_MAX_LENGTH;
	}
	if (length_a == 0) {
		if (length_b <= max_distance) {
			return length_b;
		}
		else {
			return UINT_MAX;
		}
	}
	else if (length_b == 0) {
		return UINT_MAX;
	}
	else if (length_a > length_b + max_distance) {
		/* if the query is that long, it is impossible
		 * to find a matching substring
		 */
		return UINT_MAX;
	}

	setup_initial_range(&initial, a, length_a, b, length_b, max_distance);

	result = process_range(&initial);

	return result;
}

static int has_subrange(const struct range *range)
{
	return range->first_match <= range->length_b;
}

static void setup_no_subrange(struct range *range)
{
	/* length_b + 1  is an invalid index, which signals
	 * that no matching subrange is found yet
	 */
	range->first_match = range->length_b + 1;
	range->last_match = range->length_b + 1;
}

static void setup_initial_range(struct range *initial,
	const char *a, unsigned length_a,
	const char *b, unsigned length_b,
	unsigned max_distance)
{
	size_t row_size;

	initial->a = (const unsigned char*)a;
	initial->ch = tolower(initial->a[0]);
	initial->length_a = length_a;
	initial->b = (const unsigned char*)b;
	initial->length_b = length_b;
	initial->max_distance = max_distance;
	initial->a_index = 1;

	row_size = (length_b + 2) * sizeof(*initial->row);

	initial->row = storage0;
	initial->row_prev = storage1;

	if (initial->row == NULL || initial->row_prev == NULL) {
		err(EXIT_FAILURE, "alloc");
	}
	/* Fill the top row of the matrix with zeros */
	memset(initial->row_prev, 0, row_size);

	setup_no_subrange(initial);
}

static int min_of_three(int a, int b, int c)
{
	int t;

	t = (a < b) ? a : b;
	return (t < c) ? t : c;
}

static int chars_match(const struct range *data, unsigned i)
{
	/* The characters of b start at b[0], but row[0] refers
	 * to an empty string, thus the value in
	 * row[1] is related to b[0] --- Must subtract one from that index
	 *
	 * data->ch is expected to be already lowercase during
	 * this comparison
	 */ 

	return tolower(data->b[i-1]) == data->ch;
}

static unsigned levenshtein_value(const struct range *data, unsigned i)
{
	unsigned insertion, deletion, substitution;

	if (chars_match(data, i)) {
		return data->row_prev[i-1];
	}
	else {
		insertion = data->row_prev[i] + 1;
		deletion = data->row[i-1] + 1;
		substitution = data->row_prev[i-1] + 1;

		return min_of_three(insertion, deletion, substitution);
	}
}

static int process_subrange(const struct range *range)
{
	struct range subrange;

	/* the new range to check starts at the first match,
	 * ends at well after the last match
	 */
	subrange.b = range->b + range->first_match - 1;
	subrange.length_b = range->last_match - range->first_match;
	subrange.length_b += range->length_a;
	if (subrange.length_b > range->length_b - range->first_match + 1) {
		subrange.length_b = range->length_b - range->first_match + 1;
	}

	/* the pevious characters of the query string are ignored
	 * thus decreasing the length of the remaining query
	 */
	subrange.a = range->a + 1;
	subrange.a_index = range->a_index + 1;
	subrange.length_a = range->length_a - 1;
	subrange.ch = tolower(subrange.a[0]);

	subrange.max_distance = range->max_distance;

	/* the relevant parts of the matrix are reused,
	 * knowing the parent range does not need these data anymore */
	subrange.row = range->row_prev + range->first_match - 1;
	subrange.row_prev = range->row + range->first_match - 1;

	setup_no_subrange(&subrange);

	return process_range(&subrange);
}

static int is_beyond_subrange(const struct range *range, unsigned i)
{
	/* the subrange to process should include enough tailing characters
	 * thus it is OK to process a subrange once length_a elements of row
	 * are already process after the last element lower max_distance
	 */
	return i > range->last_match + range->length_a;
}

static void update_subrange(struct range *range, unsigned i)
{
	if (range->row[i] <= range->max_distance) {

		/* if there is no subrange already observed, then start one */
		if (!has_subrange(range)) {
			range->first_match = i;
		}

		/* this is last known match at the moment */
		range->last_match = i;
	}
}

static int is_in_last_row(const struct range *range)
{
	return range->length_a == 1;
}

static unsigned process_range(struct range *range)
{
	/* TODO
	 *  This procedure is overcomplicated, needs a little refactoring
	 */

	unsigned i;
	unsigned best_result, subresult;

	best_result = UINT_MAX;

	/* Fill the first element of the row with
	 * the edit distance of the query and an empty string
	 */
	range->row[0] = range->a_index;

	for (i = 1; i <= range->length_b && best_result > 0; ++i) {
		range->row[i] = levenshtein_value(range, i);

		if (is_in_last_row(range)) {
			if (range->row[i] < best_result) {
				best_result = range->row[i];
			}
		}
		else {
			update_subrange(range, i);
			if (is_beyond_subrange(range, i)) {
				subresult = process_subrange(range);
				if (subresult < best_result) {
					best_result = subresult;
				}
				setup_no_subrange(range);
			}
		}
	}
	if (has_subrange(range)) {
		subresult = process_subrange(range);
		if (subresult < best_result) {
			best_result = subresult;
		}
	}
	return best_result;
}
