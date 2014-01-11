/*
 * Scheme 9 from Empty Space
 * By Nils M Holm, 2007-2013
 * Placed in the Public Domain
 */

/*
 * Use -DNO_SIGNALS to disable POSIX signal handlers.
 * Use -DBITS_PER_WORD_64 on 64-bit systems.
 * Use -DREALNUM to enable real number support
 *     (also add "s9-real.scm" to the heap image).
 */

#define VERSION "2013-11-26"

#define EXTERN
 #include "s9.h"
#undef EXTERN

#ifndef EXTENSIONS
 #define EXTENSIONS
#endif

int	Verbose_GC = 0;

cell	*GC_root[] = { &Program, &Symbols, &Environment, &Tmp,
			&Tmp_car, &Tmp_cdr, &Stack, &Stack_bottom,
			&State_stack, &Acc, &Trace_list, &File_list,
			NULL };

/*
 * Counting
 */

int	Run_stats, Cons_stats;

struct counter {
	int	n, n1k, n1m, n1g, n1t;
};

struct counter  Reductions,
		Conses,
		Nodes,
		Collections;

void reset_counter(struct counter *c) {
	c->n = 0;
	c->n1k = 0;
	c->n1m = 0;
	c->n1g = 0;
	c->n1t = 0;
}

void count(struct counter *c) {
	char	msg[] = "statistics counter overflow";
	c->n++;
	if (c->n >= 1000) {
		c->n -= 1000;
		c->n1k++;
		if (c->n1k >= 1000) {
			c->n1k -= 1000;
			c->n1m++;
			if (c->n1m >= 1000) {
				c->n1m -= 1000;
				c->n1g++;
				if (c->n1g >= 1000) {
					c->n1t -= 1000;
					c->n1t++;
					if (c->n1t >= 1000) {
						error(msg, NOEXPR);
					}
				}
			}
		}
	}
}

cell counter_to_list(struct counter *c) {
	cell	n, m;

	n = make_integer(c->n);
	n = cons(n, NIL);
	save(n);
	m = make_integer(c->n1k);
	n = cons(m, n);
	car(Stack) = n;
	m = make_integer(c->n1m);
	n = cons(m, n);
	car(Stack) = n;
	m = make_integer(c->n1g);
	n = cons(m, n);
	car(Stack) = n;
	m = make_integer(c->n1t);
	n = cons(m, n);
	unsave(1);
	return n;
}

cell error(char *msg, cell expr);

void flush(void) {
	fflush(Ports[Output_port]);
}

void pr_raw(char *s, int k) {
	if (Printer_limit && Printer_count > Printer_limit) {
		if (Printer_limit > 0)
			fwrite("...", 1, 3, Ports[Output_port]);
		Printer_limit = -1;
		return;
	}
	fwrite(s, 1, k, Ports[Output_port]);
	if (Output_port == 1 && s[k-1] == '\n')
		flush();
	Printer_count += k;
}

void pr(char *s) {
	if (Ports[Output_port] == NULL)
		error("output port is not open", NOEXPR);
	else
		pr_raw(s, strlen(s));
}

/*
 * Error Handling
 */

void reset_tty(void) {
#ifdef CURSES_RESET
	cell pp_curs_endwin(cell);
	pp_curs_endwin(NIL);
#endif
}

void bye(int n) {
	reset_tty();
	exit(n);
}

void print_form(cell n);

void print_error_form(cell n) {
	Printer_limit = 50;
	Printer_count = 0;
	print_form(n);
	Printer_limit = 0;
}

void print_calltrace(void) {
	int	i, j;

	for (i=0; i<Proc_max; i++)
		if (Called_procedures[i] != NIL)
			break;
	if (i == Proc_max)
		return;
	pr("call trace:");
	i = Proc_ptr;
	for (j=0; j<Proc_max; j++) {
		if (i >= Proc_max)
			i = 0;
		if (Called_procedures[i] != NIL) {
			pr(" ");
			print_form(Called_procedures[i]);
		}
		i++;
	}
	nl();
}

void reset_tty(void);

cell error(char *msg, cell expr) {
	int	oport;
	char	buf[100];

	if (Error_flag)
		return UNSPECIFIC;
	oport = Output_port;
	Output_port = Quiet_mode? 2: 1;
	Error_flag = 1;
	pr("error: ");
	if (box_value(S_loading) == TRUE) {
		if (File_list != NIL) {
			print_form(car(File_list));
			pr(": ");
		}
		sprintf(buf, "%d: ", Line_no);
		pr(buf);
	}
	pr(msg);
	if (expr != NOEXPR) {
		pr(": ");
		Error_flag = 0;
		print_error_form(expr);
		Error_flag = 1;
	}
	nl();
	print_calltrace();
	Output_port = oport;
	if (Quiet_mode)
		bye(1);
	return UNSPECIFIC;
}

void fatal(char *msg) {
	pr("fatal ");
	Error_flag = 0;
	error(msg, NOEXPR);
	bye(2);
}

/*
 * Memory Management
 */

void new_cons_segment(void) {
	Car = realloc(Car, sizeof(cell) * (Cons_pool_size+Cons_segment_size));
	Cdr = realloc(Cdr, sizeof(cell) * (Cons_pool_size+Cons_segment_size));
	Tag = realloc(Tag, Cons_pool_size + Cons_segment_size);
	if (Car == NULL || Cdr == NULL || Tag == NULL)
		fatal("new_cons_segment: out of physical memory");
	memset(&car(Cons_pool_size), 0, Cons_segment_size * sizeof(cell));
	memset(&cdr(Cons_pool_size), 0, Cons_segment_size * sizeof(cell));
	memset(&Tag[Cons_pool_size], 0, Cons_segment_size);
	Cons_pool_size += Cons_segment_size;
	Cons_segment_size = Cons_segment_size * 3 / 2;
}

void new_vec_segment(void) {
	Vectors = realloc(Vectors, sizeof(cell) *
			(Vec_pool_size + Vec_segment_size));
	if (Vectors == NULL)
		fatal("out of physical memory");
	memset(&Vectors[Vec_pool_size], 0, Vec_segment_size * sizeof(cell));
	Vec_pool_size += Vec_segment_size;
	Vec_segment_size = Vec_segment_size * 3 / 2;
}

/*
 * Mark nodes which can be accessed through N.
 * Using the Deutsch/Schorr/Waite pointer reversal algorithm.
 * S0: M==0, S==0, unvisited, process CAR (vectors: process 1st slot);
 * S1: M==1, S==1, CAR visited, process CDR (vectors: process next slot);
 * S2: M==1, S==0, completely visited, return to parent.
 */

void mark(cell n) {
	cell	p, parent, *v;
	int	i;

	parent = NIL;	/* Initially, there is no parent node */
	while (1) {
		if (special_value_p(n) || Tag[n] & MARK_TAG) {
			if (parent == NIL)
				break;
			if (Tag[parent] & VECTOR_TAG) {	/* S1 --> S1|done */
				i = vector_index(parent);
				v = vector(parent);
				if (Tag[parent] & STATE_TAG &&
				    i+1 < vector_len(parent)
				) {			/* S1 --> S1 */
					p = v[i+1];
					v[i+1] = v[i];
					v[i] = n;
					n = p;
					vector_index(parent) = i+1;
				}
				else {			/* S1 --> done */
					p = parent;
					parent = v[i];
					v[i] = n;
					n = p;
				}
			}
			else if (Tag[parent] & STATE_TAG) {	/* S1 --> S2 */
				p = cdr(parent);
				cdr(parent) = car(parent);
				car(parent) = n;
				Tag[parent] &= ~STATE_TAG;
				Tag[parent] |=  MARK_TAG;
				n = p;
			}
			else {				/* S2 --> done */
				p = parent;
				parent = cdr(p);
				cdr(p) = n;
				n = p;
			}
		}
		else {
			if (Tag[n] & VECTOR_TAG) {	/* S0 --> S1|S2 */
				Tag[n] |= MARK_TAG;
				/* Tag[n] &= ~STATE_TAG; */
				vector_link(n) = n;
				if (car(n) == T_VECTOR && vector_len(n) != 0) {
					Tag[n] |= STATE_TAG;
					vector_index(n) = 0;
					v = vector(n);
					p = v[0];
					v[0] = parent;
					parent = n;
					n = p;
				}
			}
			else if (Tag[n] & ATOM_TAG) {	/* S0 --> S2 */
				if (input_port_p(n) || output_port_p(n))
					Port_flags[port_no(n)] |= USED_TAG;
				p = cdr(n);
				cdr(n) = parent;
				/*Tag[n] &= ~STATE_TAG;*/
				parent = n;
				n = p;
				Tag[parent] |= MARK_TAG;
			}
			else {				/* S0 --> S1 */
				p = car(n);
				car(n) = parent;
				Tag[n] |= MARK_TAG;
				parent = n;
				n = p;
				Tag[parent] |= STATE_TAG;
			}
		}
	}
}

/* Mark and sweep GC. */
int gc(void) {
	int	i, k;
	char	buf[100];

	if (Run_stats)
		count(&Collections);
	for (i=0; i<MAX_PORTS; i++)
		if (Port_flags[i] & LOCK_TAG)
			Port_flags[i] |= USED_TAG;
		else
			Port_flags[i] &= ~USED_TAG;
	for (i=0; GC_root[i] != NULL; i++)
		mark(GC_root[i][0]);
	k = 0;
	Free_list = NIL;
	for (i=0; i<Cons_pool_size; i++) {
		if (!(Tag[i] & MARK_TAG)) {
			cdr(i) = Free_list;
			Free_list = i;
			k++;
		}
		else {
			Tag[i] &= ~MARK_TAG;
		}
	}
	for (i=0; i<MAX_PORTS; i++) {
		if (!(Port_flags[i] & USED_TAG) && Ports[i] != NULL) {
			fclose(Ports[i]);
			Ports[i] = NULL;
		}
	}
	if (Verbose_GC > 1) {
		sprintf(buf, "GC: %d nodes reclaimed", k);
		pr(buf); nl();
	}
	return k;
}

/* Allocate a fresh node and initialize with PCAR,PCDR,PTAG. */
cell cons3(cell pcar, cell pcdr, int ptag) {
	cell	n;
	int	k;
	char	buf[100];

	if (Run_stats) {
		count(&Nodes);
		if (Cons_stats)
			count(&Conses);
	}
	if (Free_list == NIL) {
		if (ptag == 0)
			Tmp_car = pcar;
		if (ptag != VECTOR_TAG && ptag != PORT_TAG)
			Tmp_cdr = pcdr;
		k = gc();
		/*
		 * Performance increases dramatically if we
		 * do not wait for the pool to run dry.
		 * In fact, don't even let it come close to that.
		 */
		if (k < Cons_pool_size / 2) {
			if (	Memory_limit_kn &&
				Cons_pool_size + Cons_segment_size
					> Memory_limit_kn
			) {
				error("hit memory limit", NOEXPR);
			}
			else {
				new_cons_segment();
				if (Verbose_GC) {
					sprintf(buf,
						"GC: new segment,"
						 " nodes = %d,"
						 " next segment = %d",
						Cons_pool_size,
						Cons_segment_size);
					pr(buf); nl();
				}
				gc();
			}
		}
		Tmp_car = Tmp_cdr = NIL;
	}
	if (Free_list == NIL)
		fatal("cons3: failed to recover from low memory condition");
	n = Free_list;
	Free_list = cdr(Free_list);
	car(n) = pcar;
	cdr(n) = pcdr;
	Tag[n] = ptag;
	return n;
}

/* Mark all vectors unused */
void unmark_vectors(void) {
	int	p, k, link;

	p = 0;
	while (p < Free_vecs) {
		link = p;
		k = Vectors[p + RAW_VECTOR_SIZE];
		p += vector_size(k);
		Vectors[link] = NIL;
	}
}

/* In situ vector pool garbage collection and compaction */
int gcv(void) {
	int	v, k, to, from;
	char	buf[100];

	unmark_vectors();
	gc();		/* re-mark live vectors */
	to = from = 0;
	while (from < Free_vecs) {
		v = Vectors[from + RAW_VECTOR_SIZE];
		k = vector_size(v);
		if (Vectors[from + RAW_VECTOR_LINK] != NIL) {
			if (to != from) {
				memmove(&Vectors[to], &Vectors[from],
					k * sizeof(cell));
				cdr(Vectors[to + RAW_VECTOR_LINK]) =
					to + RAW_VECTOR_DATA;
			}
			to += k;
		}
		from += k;
	}
	k = Free_vecs - to;
	if (Verbose_GC > 1) {
		sprintf(buf, "GC: gcv: %d cells reclaimed", k);
		pr(buf); nl();
	}
	Free_vecs = to;
	return k;
}

/* Allocate vector from pool */
cell new_vec(cell type, int size) {
	cell	n;
	int	v, wsize;
	char	buf[100];

	wsize = vector_size(size);
	if (Free_vecs + wsize >= Vec_pool_size) {
		gcv();
		while (	Free_vecs + wsize >=
			Vec_pool_size - Vec_pool_size / 2
		) {
			if (	Memory_limit_kn &&
				Vec_pool_size + Vec_segment_size
					> Memory_limit_kn
			) {
				error("hit memory limit", NOEXPR);
				break;
			}
			else {
				new_vec_segment();
				gcv();
				if (Verbose_GC) {
					sprintf(buf,
						"GC: new_vec: new segment,"
						 " cells = %d",
						Vec_pool_size);
					pr(buf); nl();
				}
			}
		}
	}
	if (Free_vecs + wsize >= Vec_pool_size)
		fatal("new_vec: failed to recover from low memory condition");
	v = Free_vecs;
	Free_vecs += wsize;
	n = cons3(type, v + RAW_VECTOR_DATA, VECTOR_TAG);
	Vectors[v + RAW_VECTOR_LINK] = n;
	Vectors[v + RAW_VECTOR_INDEX] = 0;
	Vectors[v + RAW_VECTOR_SIZE] = size;
	return n;
}

/* Pop K nodes off the Stack, return last one. */
cell unsave(int k) {
	cell	n = NIL; /*LINT*/

	while (k) {
		if (Stack == NIL)
			fatal("unsave: stack underflow");
		n = car(Stack);
		Stack = cdr(Stack);
		k--;
	}
	return n;
}

/*
 * Reader
 */

cell find_symbol(char *s) {
	cell	y;

	y = Symbols;
	while (y != NIL) {
		if (!strcmp(symbol_name(car(y)), s))
			return car(y);
		y = cdr(y);
	}
	return NIL;
}

cell make_symbol(char *s, int k) {
	cell	n;

	n = new_vec(T_SYMBOL, k+1);
	strcpy(symbol_name(n), s);
	return n;
}

cell symbol_ref(char *s) {
	cell	y, new;

	y = find_symbol(s);
	if (y != NIL)
		return y;
	new = make_symbol(s, (int) strlen(s));
	Symbols = cons(new, Symbols);
	return car(Symbols);
}

cell read_form(int flags);

cell read_list(int flags, int delim) {
	cell	n,	/* Node read */
		m,	/* List */
		a;	/* Used to append nodes to m */
	int	c;	/* Member counter */
	cell	new;
	char	badpair[] = "malformed pair";

	if (++Level > MAX_IO_DEPTH) {
		error("reader: too many nested lists or vectors", NOEXPR);
		return NIL;
	}
	m = cons3(NIL, NIL, flags);	/* root */
	save(m);
	a = NIL;
	c = 0;
	while (1) {
		if (Error_flag) {
			unsave(1);
			return NIL;
		}
		n = read_form(flags);
		if (n == END_OF_FILE)  {
			if (Load_level) {
				unsave(1);
				return END_OF_FILE;
			}
			error("missing ')'", NOEXPR);
		}
		if (n == DOT) {
			if (c < 1) {
				error(badpair, NOEXPR);
				continue;
			}
			n = read_form(flags);
			cdr(a) = n;
			if (n == delim || read_form(flags) != delim) {
				error(badpair, NOEXPR);
				continue;
			}
			unsave(1);
			Level--;
			return m;
		}
		if (n == RPAREN || n == RBRACK) {
			if (n != delim)
				error(n == RPAREN?
				  "list starting with `[' ended with `)'":
				  "list starting with `(' ended with `]'",
				  NOEXPR);
			break;
		}
		if (a == NIL)
			a = m;		/* First member: insert at root */
		else
			a = cdr(a);	/* Subsequent members: append */
		car(a) = n;
		new = cons3(NIL, NIL, flags); /* Space for next member */
		cdr(a) = new;
		c++;
	}
	Level--;
	if (a != NIL)
		cdr(a) = NIL;	/* Remove trailing empty node */
	unsave(1);
	return c? m: NIL;
}

cell quote(cell n, cell quotation) {
	cell	q;

	q = cons(n, NIL);
	return cons(quotation, q);
}

#ifdef REALNUM

 #include "s9-real.c"
 #define SYM_CHARS	"!@#$%^&*-/_+=~.?<>:"

#else /* !REALNUM */

 #define string_to_number(x)	string_to_bignum(x)
 #define SYM_CHARS	"!@$%^&*-/_+=~.?<>:"

 #define bignum_to_real(x)	(x)
 #define integer_argument(a,b)	(b)
 #define real_abs		bignum_abs
 #define real_add		bignum_add
 #define real_equal_p		bignum_equal_p
 #define real_integer_p		integer_p
 #define real_less_p		bignum_less_p
 #define real_multiply		bignum_multiply
 #define real_negate		bignum_negate
 #define real_negative_p	_bignum_negative_p
 #define real_positive_p	_bignum_positive_p
 #define real_subtract		bignum_subtract
 #define real_zero_p		_bignum_zero_p

 int string_numeric_p(char *s) {
	int	i;
	int	got_d, got_e, got_dp, got_s;

	i = 0;
	got_s = 0;
	got_d = 0;
	got_dp = 0;
	got_e = 0;
	if (s[0] == '+' || s[0] == '-') {
		i = 1;
		got_s = 1;
	}
	if (!s[i])
		return 0;
	while (s[i]) {
		if (!isdigit(s[i]))
			return 0;
		i++;
	}
	return 1;
 }

 cell string_to_bignum(char *s) {
	cell	n, v;
	int	k, j, sign;

	sign = 1;
	if (s[0] == '-') {
		s++;
		sign = -1;
	}
	else if (s[0] == '+') {
		s++;
	}
	/* plan9's atol() interprets leading 0 as octal! */
	while (s[0] == '0' && s[1])
		s++;
	k = (int) strlen(s);
	n = NIL;
	while (k) {
		j = k <= DIGITS_PER_WORD? k: DIGITS_PER_WORD;
		v = atol(&s[k-j]);
		s[k-j] = 0;
		k -= j;
		if (k == 0)
			v *= sign;
		n = new_atom(v, n);
	}
	return new_atom(T_INTEGER, n);
 }

#endif /* !REALNUM */

/* Create a character literal. */
cell make_char(int x) {
	cell n;

	n = new_atom(x & 0xff, NIL);
	return new_atom(T_CHAR, n);
}

int strcmp_ci(char *s1, char *s2) {
	int	c1, c2;

	while (1) {
		c1 = tolower((int) *s1++);
		c2 = tolower((int) *s2++);
		if (!c1 || !c2 || c1 != c2)
			break;
	}
	return c1<c2? -1: c1>c2? 1: 0;
}

/* Read a character literal. */
cell read_character(void) {
	char	buf[10], msg[50];
	int	i, c = 0; /*LINT*/

	for (i=0; i<sizeof(buf)-1; i++) {
		c = read_c();
		if (i > 0 && !isalpha(c))
			break;
		buf[i] = c;
	}
	reject(c);
	buf[i] = 0;
	if (i == 0)
		c = ' ';
	else if (i == 1)
		c = buf[0];
	else if (!strcmp_ci(buf, "space"))
		c = ' ';
	else if (!strcmp_ci(buf, "newline"))
		c = '\n';
	else {
		sprintf(msg, "unknown character: #\\%s", buf);
		error(msg, NOEXPR);
		c = 0;
	}
	return make_char(c);
}

/* Create a string; K = length */
cell make_string(char *s, int k) {
	cell	n;

	n = new_vec(T_STRING, k+1);
	strncpy(string(n), s, k+1);
	return n;
}

/* Read a string literal. */
cell read_string(void) {
	char	s[TOKEN_LENGTH+1];
	cell	n;
	int	c, i, q;
	int	inv;

	i = 0;
	q = 0;
	c = read_c();
	inv = 0;
	while (q || c != '"') {
		if (c == EOF)
			error("missing '\"' in string literal", NOEXPR);
		if (Error_flag)
			break;
		if (i >= TOKEN_LENGTH-2) {
			error("string literal too long", NOEXPR);
			i--;
		}
		if (q && c != '"' && c != '\\') {
			s[i++] = '\\';
			inv = 1;
		}
		s[i] = c;
		q = !q && c == '\\';
		if (!q)
			i++;
		c = read_c();
	}
	s[i] = 0;
	n = make_string(s, i);
	Tag[n] |= CONST_TAG;
	if (inv)
		error("invalid escape sequence in string", n);
	return n;
}

/* Report unreadable object */
cell unreadable(void) {
	int	c, i;
	char	buf[TOKEN_LENGTH];
	int	d;

	strcpy(buf, "#<");
	i = 2;
	while (1) {
		c = read_c_ci();
		if (c == '>' || c == '\n') {
			if (c == '\n')
				Line_no++;
			break;
		}
		if (i < TOKEN_LENGTH-2)
			buf[i++] = c;
	}
	buf[i++] = '>';
	buf[i] = 0;
	d = Displaying;
	Displaying = 1;
	error("unreadable object", make_string(buf, i));
	Displaying = d;
	return NIL;
}

#define separator(c) \
	((c) == ' '  || (c) == '\t' || (c) == '\n' || \
	 (c) == '\r' || (c) == '('  || (c) == ')'  || \
	 (c) == ';'  || (c) == '\'' || (c) == '`'  || \
	 (c) == ','  || (c) == '"'  || (c) == '['  || \
	 (c) == ']'  || (c) == EOF)

#define is_symbolic(c) \
	(isalpha(c) ||				\
	 isdigit(c) ||				\
	 strchr(SYM_CHARS, (c)))

cell funny_char(char *msg, int c) {
	char	buf[128];

	if (isprint(c))
		return error(msg, make_char(c));
	sprintf(buf, "%s, code", msg);
	return error(buf, make_integer(c));
}

cell read_symbol_or_number(int c) {
	char	s[TOKEN_LENGTH];
	int	i, funny = 0;

	i = 0;
	while (!separator(c)) {
		if (!is_symbolic(c))
			funny = c;
		if (i >= TOKEN_LENGTH-2) {
			error("symbol too long", NOEXPR);
			i--;
		}
		s[i] = c;
		i++;
		c = read_c_ci();
	}
	s[i] = 0;
	reject(c);
	if (funny)
		return funny_char("funny character in symbol", funny);
	if (string_numeric_p(s))
		return string_to_number(s);
	if (!strcmp(s, "define-macro"))
		return S_define_syntax;
	return symbol_ref(s);
}

cell list_to_vector(cell m, char *msg, int flags) {
	cell	n, vec;
	int	k;
	cell	*p;

	k = 0;
	for (n = m; n != NIL; n = cdr(n)) {
		if (atom_p(n))
			return error(msg, m);
		k++;
	}
	vec = new_vec(T_VECTOR, k*sizeof(cell));
	Tag[vec] |= flags;
	p = vector(vec);
	for (n = m; n != NIL; n = cdr(n)) {
		*p = car(n);
		p++;
	}
	return vec;
}

cell read_vector(void) {
	cell	n;

	n = read_list(0, RPAREN);
	save(n);
	n = list_to_vector(n, "invalid vector syntax", CONST_TAG);
	unsave(1);
	return n;
}

cell meta_command(void) {
	int	c, cmd, i;
	cell	n, cmdsym;
	char	s[128];

	cmd = read_c_ci();
	c = read_c();
	while (c == ' ')
		c = read_c();
	i = 0;
	while (c != '\n' && c != EOF) {
		if (i < sizeof(s) - 2)
			s[i++] = c;
		c = read_c();
	}
	reject(c);
	s[i] = 0;
	n = make_string(s, strlen(s));
	n = i == 0? NIL: cons(n, NIL);
	switch (cmd) {
	case 'a':	cmdsym = symbol_ref("apropos"); break;
	case 'h':	cmdsym = symbol_ref("help"); break;
	case 'l':	cmdsym = symbol_ref("load-from-library"); break;
	case 'q':	cmdsym = symbol_ref("sys:exit"); break;
	default: 	pr(",a = apropos"); nl();
			pr(",h = help"); nl();
			pr(",l = load-from-library"); nl();
			pr(",q = sys:exit"); nl();
			return UNSPECIFIC;
	}
	return cons(cmdsym, n);
}

int block_comment(void) {
	int	n, c, state = 0;

	for (n=1; n; ) {
		c = read_c_ci();
		switch (c) {
		case EOF:
			error("missing |#", NOEXPR);
			return 0;
		case '|':
			switch (state) {
			case 1:		n++; state = 0; break;
			default:	state = -1; break;
			}
			break;
		case '#':
			switch (state) {
			case -1:	n--; state = 0; break;
			default:	state = 1; break;
			}
			break;
		case '\n':
			Line_no++;
			state = 0;
			break;
		default:
			state = 0;
			break;
		}
	}
	return read_c_ci();
}

int closing_paren(void) {
	int c = read_c_ci();

	reject(c);
	return c == ')';
}

cell bignum_read(char *pre, int radix);

cell read_form(int flags) {
	char	buf[50];
	int	c, c2;

	c = read_c_ci();
	while (1) {	/* Skip over spaces and comments */
		while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (c == '\n')
				Line_no++;
			if (Error_flag)
				return NIL;
			c = read_c_ci();
		}
		if (c == '#') {
			c = read_c_ci();
			if (c == '!') {
				/**/
			}
			else if (c == '|') {
				c = block_comment();
				continue;
			}
			else {
				reject(c);
				c = '#';
				break;
			}
		}
		else if (c != ';')
			break;
		while (!Error_flag && c != '\n' && c != EOF)
			c = read_c_ci();
		if (Error_flag)
			return UNSPECIFIC;
	}
	if (c == EOF)
		return END_OF_FILE;
	if (Error_flag)
		return UNSPECIFIC;
	if (c == '(') {
		return read_list(flags, RPAREN);
	}
	else if (c == '[') {
		return read_list(flags, RBRACK);
	}
	else if (c == '\'' || c == '`') {
		cell	n;

		if (closing_paren())
			return error("missing form after \"'\" or \"`\"",
					NOEXPR);
		Level++;
		n = quote(read_form(CONST_TAG), c=='`'? S_quasiquote:
							S_quote);
		Level--;
		return n;
	}
	else if (c == ',') {
		if (closing_paren())
			return error("missing form after \",\"",
					NOEXPR);
		c = read_c_ci();
		if (c == '@') {
			return quote(read_form(0), S_unquote_splicing);
		}
		else {
			reject(c);
			if (!Level)
				return meta_command();
			return quote(read_form(0), S_unquote);
		}
	}
	else if (c == '#') {
		c = read_c_ci();
		switch (c) {
		case 'f':	return FALSE;
		case 't':	return TRUE;
		case '\\':	return read_character();
		case '(':	return read_vector();
		case 'b':	return bignum_read("#b", 2);
		case 'd':	return bignum_read("#d", 10);
		case 'o':	return bignum_read("#o", 8);
		case 'x':	return bignum_read("#x", 16);
#ifdef REALNUM
		case 'e':	return read_real_number(0);
		case 'i':	return read_real_number(1);
#endif
		case '<':	return unreadable();
		default:	sprintf(buf, "unknown # syntax: #%c", c);
				return error(buf, NOEXPR);
		}
	}
	else if (c == '"') {
		return read_string();
	}
	else if (c == ')') {
		if (!Level) return error("unexpected ')'", NOEXPR);
		return RPAREN;
	}
	else if (c == ']') {
		if (!Level) return error("unexpected ']'", NOEXPR);
		return RBRACK;
	}
	else if (c == '.') {
		c2 = read_c_ci();
		reject(c2);
		if (separator(c2)) {
			if (!Level)
				return error("unexpected '.'", NOEXPR);
			return DOT;
		}
		return read_symbol_or_number(c);
	}
	else if (is_symbolic(c)) {
		return read_symbol_or_number(c);
	}
	else {
		return funny_char("funny input character", c);
	}
}

cell xread(void) {
	if (Ports[Input_port] == NULL)
		return error("input port is not open", NOEXPR);
	Level = 0;
	return read_form(0);
}

/*
 * Printer
 */

char *ntoa(char *b, cell x, int w) {
	char	buf[40];
	int	i = 0, neg = 0;
	char	*p = &buf[sizeof(buf)-1];

	if (x < 0) {
		x = -x;
		neg = 1;
	}
	*p = 0;
	while (x || i == 0) {
		i++;
		if (i >= sizeof(buf)-1)
			fatal("ntoa: number too big");
		p--;
		*p = x % 10 + '0';
		x = x / 10;
	}
	while (i < (w-neg) && i < sizeof(buf)-1) {
		i++;
		p--;
		*p = '0';
	}
	if (neg) {
		if (i >= sizeof(buf)-1)
			fatal("ntoa: number too big");
		p--;
		*p = '-';
	}
	strcpy(b, p);
	return b;
}

/* Print bignum integer. */
int print_integer(cell n) {
	int	first;
	char	buf[DIGITS_PER_WORD+2];

	if (!integer_p(n))
		return 0;
	n = cdr(n);
	first = 1;
	while (n != NIL) {
		pr(ntoa(buf, car(n), first? 0: DIGITS_PER_WORD));
		n = cdr(n);
		first = 0;
	}
	return 1;
}

/* Print expressions of the form (QUOTE X) as 'X. */
int print_quoted(cell n) {
	if (	car(n) == S_quote &&
		cdr(n) != NIL &&
		cddr(n) == NIL
	) {
		pr("'");
		print_form(cadr(n));
		return 1;
	}
	return 0;
}

int print_procedure(cell n) {
	if (procedure_p(n)) {
		pr("#<procedure ");
		print_form(cadr(n));
		pr(">");
		return 1;
	}
	return 0;
}

int print_continuation(cell n) {
	if (continuation_p(n)) {
		pr("#<continuation>");
		return 1;
	}
	return 0;
}

int print_char(cell n) {
	char	b[1];
	int	c;

	if (!char_p(n))
		return 0;
	if (!Displaying)
		pr("#\\");
	c = cadr(n);
	if (!Displaying && c == ' ')
		pr("space");
	else if (!Displaying && c == '\n')
		pr("newline");
	else {
		b[0] = c;
		pr_raw(b, 1);
	}
	return 1;
}

int print_string(cell n) {
	char	b[1];
	int	k;
	char	*s;

	if (!string_p(n))
		return 0;
	if (!Displaying)
		pr("\"");
	s = string(n);
	k = string_len(n)-1;
	while (k) {
		b[0] = *s++;
		if (!Displaying && (b[0] == '"' || b[0] == '\\'))
			pr("\\");
		pr_raw(b, 1);
		k--;
	}
	if (!Displaying)
		pr("\"");
	return 1;
}

int print_symbol(cell n) {
	char	b[2];
	int	k;
	char	*s;

	if (!symbol_p(n))
		return 0;
	s = symbol_name(n);
	k = symbol_len(n)-1;
	b[1] = 0;
	while (k) {
		b[0] = *s++;
		pr(b);
		k--;
	}
	return 1;
}

int print_primitive(cell n) {
	PRIM	*p;

	if (!primitive_p(n))
		return 0;
	pr("#<primitive ");
	p = (PRIM *) cadr(n);
	pr(p->name);
	pr(">");
	return 1;
}

int print_syntax(cell n) {
	if (!syntax_p(n))
		return 0;
	pr("#<syntax>");
	return 1;
}

int print_vector(cell n) {
	cell	*p;
	int	k;

	if (!vector_p(n))
		return 0;
	pr("#(");
	p = vector(n);
	k = vector_len(n);
	while (k--) {
		print_form(*p++);
		if (k)
			pr(" ");
	}
	pr(")");
	return 1;
}

int print_port(cell n) {
	char	buf[100];

	if (!input_port_p(n) && !output_port_p(n))
		return 0;
	sprintf(buf, "#<%s-port %d>",
		input_port_p(n)? "input": "output",
		(int) port_no(n));
	pr(buf);
	return 1;
}

void _print_form(cell n, int depth) {
	if (Ports[Output_port] == NULL) {
		error("output port is not open", NOEXPR);
		return;
	}
	if (depth > MAX_IO_DEPTH) {
		error("printer: too many nested lists or vectors", NOEXPR);
		return;
	}
	if (n == NIL) {
		pr("()");
	}
	else if (eof_p(n)) {
		pr("#<eof>");
	}
	else if (n == NAN) {
		pr("#<nan>");
	}
	else if (n == FALSE) {
		pr("#f");
	}
	else if (n == TRUE) {
		pr("#t");
	}
	else if (undefined_p(n)) {
		pr("#<undefined>");
	}
	else if (unspecific_p(n)) {
		pr("#<unspecific>");
	}
	else {
		if (print_char(n)) return;
		if (print_procedure(n)) return;
		if (print_continuation(n)) return;
#ifdef REALNUM
		if (print_real(n)) return;
#endif
		if (print_integer(n)) return;
		if (print_primitive(n)) return;
		if (print_quoted(n)) return;
		if (print_string(n)) return;
		if (print_symbol(n)) return;
		if (print_syntax(n)) return;
		if (print_vector(n)) return;
		if (print_port(n)) return;
		pr("(");
		while (n != NIL) {
			if (Error_flag) return;
			if (Printer_limit && Printer_count > Printer_limit)
				return;
			_print_form(car(n), depth+1);
			if (Error_flag) return;
			n = cdr(n);
			if (n != NIL && atom_p(n)) {
				pr(" . ");
				_print_form(n, depth+1);
				n = NIL;
			}
			if (n != NIL) pr(" ");
		}
		pr(")");
	}
}

void print_form(cell n) {
	_print_form(n, 0);
}

/*
 * Special Form Handlers
 */

int proper_list_p(cell n) {
	while (pair_p(n))
		n = cdr(n);
	return n == NIL;
}

int length(cell n) {
	int	k = 0;

	while (n != NIL) {
		k++;
		n = cdr(n);
	}
	return k;
}

cell append_b(cell a, cell b) {
	cell	p, last = NIL;

	if (a == NIL)
		return b;
	p = a;
	while (p != NIL) {
		if (atom_p(p))
			fatal("append!: improper list");
		last = p;
		p = cdr(p);
	}
	cdr(last) = b;
	return a;
}

cell flat_copy(cell n, cell *lastp) {
	cell	a, m, last, new;

	if (n == NIL) {
		if (lastp != NULL)
			*lastp = NIL;
		return NIL;
	}
	m = cons3(NIL, NIL, Tag[n]);
	save(m);
	a = m;
	while (n != NIL) {
		car(a) = car(n);
		last = a;
		n = cdr(n);
		if (n != NIL) {
			new = cons3(NIL, NIL, Tag[n]);
			cdr(a) = new;
			a = cdr(a);
		}
	}
	unsave(1);
	if (lastp != NULL)
		*lastp = last;
	return m;
}

int argument_list_p(cell n) {
	if (n == NIL || symbol_p(n))
		return 1;
	if (atom_p(n))
		return 0;
	while (pair_p(n)) {
		if (!symbol_p(car(n)))
			return 0;
		n = cdr(n);
	}
	return n == NIL || symbol_p(n);
}

#define hash(s, h) \
	do {					\
		h = 0;				\
		while (*s)			\
			h = (h<<3) ^ *s++;	\
	} while (0)

int hash_size(int n) {
	if (n < 5) return 5;
	if (n < 11) return 11;
	if (n < 23) return 23;
	if (n < 47) return 47;
	if (n < 97) return 97;
	if (n < 199) return 199;
	if (n < 499) return 499;
	if (n < 997) return 997;
	if (n < 9973) return 9973;
	return 19997;
}

void rehash(cell e) {
	unsigned int	i;
	cell		p, *v, new;
	unsigned int	h, k = hash_size(length(e));
	char		*s;

	if (Program == NIL || k < HASH_THRESHOLD)
		return;
	new = new_vec(T_VECTOR, k * sizeof(cell));
	car(e) = new;
	v = vector(car(e));
	for (i=0; i<k; i++)
		v[i] = NIL;
	p = cdr(e);
	while (p != NIL) {
		s = symbol_name(caar(p));
		hash(s, h);
		new = cons(car(p), v[h%k]);
		v = vector(car(e));
		v[h%k] = new;
		p = cdr(p);
	}
}

cell extend(cell v, cell a, cell e) {
	cell	n, new;

	n = make_binding(v, a);
	new = cons(n, cdr(e));
	cdr(e) = new;
	if (box_value(S_loading) == FALSE)
		rehash(e);
	return e;
}

cell make_env(cell rib, cell env) {
	cell	e;

	Tmp = env;
	rib = cons(NIL, rib);
	e = cons(rib, env);
	Tmp = NIL;
	if (length(rib) >= HASH_THRESHOLD) {
		save(e);
		rehash(rib);
		unsave(1);
	}
	return e;
}

cell try_hash(cell v, cell e) {
	cell		*hv, p;
	unsigned int	h, k;
	char		*s;

	if (e == NIL || car(e) == NIL)
		return NIL;
	hv = vector(car(e));
	k = vector_len(car(e));
	s = symbol_name(v);
	hash(s, h);
	p = hv[h%k];
	while (p != NIL) {
		if (caar(p) == v)
			return car(p);
		p = cdr(p);
	}
	return NIL;
}

cell lookup(cell v, cell env, int req) {
	cell	e, n;

	while (env != NIL) {
		e = car(env);
		n = try_hash(v, e);
		if (n != NIL)
			return n;
		if (e != NIL)
			e = cdr(e);
		while (e != NIL) {
			if (v == caar(e))
				return car(e);
			e = cdr(e);
		}
		env = cdr(env);
	}
	if (!req)
		return NIL;
	if (special_p(v))
		error("invalid syntax", v);
	else
		error("symbol not bound", v);
	return NIL;
}

cell too_few_args(int n) {
	return error("too few arguments", n);
}

cell too_many_args(int n) {
	return error("too many arguments", n);
}

/* Set up sequence for AND, BEGIN, OR. */
cell make_sequence(int state, cell neutral, cell x, int *pc, int *ps) {
	if (cdr(x) == NIL) {
		return neutral;
	}
	else if (cddr(x) == NIL) {
		*pc = 1;
		return cadr(x);
	}
	else {
		*pc = 2;
		*ps = state;
		save(cdr(x));
		return cadr(x);
	}
}

#define sf_and(x, pc, ps) \
	make_sequence(EV_AND, TRUE, x, pc, ps)

#define sf_begin(x, pc, ps) \
	make_sequence(EV_BEGIN, UNSPECIFIC, x, pc, ps)

cell sf_cond(cell x, int *pc, int *ps) {
	cell	clauses, p;

	clauses = cdr(x);
	p = clauses;
	while (p != NIL) {
		if (atom_p(car(p)))
			return error("cond: invalid syntax", car(p));
		p = cdr(p);
	}
	if (clauses == NIL)
		return UNSPECIFIC;
	if (caar(clauses) == S_else && cdr(clauses) == NIL) {
		p = cons(TRUE, cdar(clauses));
		clauses = cons(p, cdr(clauses));
	}
	save(clauses);
	*pc = 2;
	*ps = EV_COND;
	return caar(clauses);
}

cell sf_if(cell x, int *pc, int *ps) {
	cell	m, new;

	m = cdr(x);
	if (m == NIL || cdr(m) == NIL)
		return too_few_args(x);
	if (cddr(m) != NIL && cdddr(m) != NIL)
		return too_many_args(x);
	if (cddr(m) == NIL) {
		new = cons(UNSPECIFIC, NIL);
		cddr(m) = new;
	}
	save(m);
	*pc = 2;
	*ps = EV_IF_PRED;
	return car(m);
}

cell gensym(char *prefix);

cell make_temporaries(cell x) {
	cell	n, v;

	n = NIL;
	save(n);
	while (x != NIL) {
		v = gensym("g");
		n = cons(v, n);
		car(Stack) = n;
		x = cdr(x);
	}
	unsave(1);
	return n;
}

/*
 * Return (begin (set! x0 t0)
 *               ...
 *               (set! xN tN))
 */
cell make_assignments(cell x, cell t) {
	cell	n, asg;

	n = NIL;
	save(n);
	while (x != NIL) {
		asg = cons(car(t), NIL);
		asg = cons(car(x), asg);
		asg = cons(S_set_b, asg);
		n = cons(asg, n);
		car(Stack) = n;
		x = cdr(x);
		t = cdr(t);
	}
	unsave(1);
	return cons(S_begin, n);
}

cell make_undefineds(cell x) {
	cell	n;

	n = NIL;
	while (x != NIL) {
		n = cons(UNDEFINED, n);
		x = cdr(x);
	}
	return n;
}

/* Return ((lambda (v0 ...)
 *           ((lambda (t0 ...)
 *              (begin (set! v0 t0)
 *                     ...
 *                     body))
 *            a0 ...))
 *         #<undefined>
 *         ...)
 */
cell make_recursive_lambda(cell v, cell a, cell body) {
	cell	t, n;

	t = make_temporaries(v);
	save(t);
	body = append_b(make_assignments(v, t), body);
	body = cons(body, NIL);
	n = cons(t, body);
	n = cons(S_lambda, n);
	n = cons(n, a);
	n = cons(n, NIL);
	n = cons(v, n);
	n = cons(S_lambda, n);
	save(n);
	n = cons(n, make_undefineds(v));
	unsave(2);
	return n;
}

enum { VARS, VALS };

/* Extract variables or arguments from a set of DEFINEs. */
cell extract_from_defines(cell x, int part, cell *restp) {
	cell	a, n, new;
	int	k;

	a = NIL;
	while (x != NIL) {
		if (atom_p(x) || atom_p(car(x)) || caar(x) != S_define)
			break;
		n = car(x);
		if (	!proper_list_p(n) ||
			(k = length(n)) < 3 ||
			!argument_list_p(cadr(n)) ||
			(symbol_p(cadr(n)) && k > 3)
		)
			return error("define: invalid syntax", n);
		if (pair_p(cadr(n))) {
			/* (define (proc vars) ...) */
			if (part == VARS) {
				a = cons(caadr(n), a);
			}
			else {
				a = cons(NIL, a);
				save(a);
				new = cons(cdadr(n), cddr(n));
				new = cons(S_lambda, new);
				car(a) = new;
				unsave(1);
			}
		}
		else {
			a = cons(part==VARS? cadr(n): caddr(n), a);
		}
		x = cdr(x);
	}
	*restp = x;
	return a;
}

/*
 * Rewrite local DEFINEs using LAMBDA and SET!.
 * This is semantically equivalent to:
 *
 * (lambda ()        --->  (lambda ()
 *   (define v0 a0)          (letrec ((v0 a0)
 *   ...                              ...)
 *   body)                     body)
 */
cell resolve_local_defines(int x) {
	cell	v, a, n, rest;

	a = extract_from_defines(x, VALS, &rest);
	if (Error_flag)
		return NIL;
	save(a);
	v = extract_from_defines(x, VARS, &rest);
	save(v);
	if (rest == NIL)
		rest = cons(UNSPECIFIC, NIL);
	save(rest);
	n = make_recursive_lambda(v, a, rest);
	unsave(3);
	return n;
}

cell sf_lambda(cell x) {
	cell	n;
	int	k;

	k = length(x);
	if (k < 3)
		return too_few_args(x);
	if (!argument_list_p(cadr(x)))
		return error("malformed argument list", cadr(x));
	if (pair_p(caddr(x)) && caaddr(x) == S_define)
		n = resolve_local_defines(cddr(x));
	else if (k > 3)
		n = cons(S_begin, cddr(x));
	else
		n = caddr(x);
	n = cons(n, Environment);
	n = cons(cadr(x), n);
	return new_atom(T_PROCEDURE, n);
}

cell sf_quote(cell x) {
	if (cdr(x) == NIL) return too_few_args(x);
	if (cddr(x) != NIL) return too_many_args(x);
	return cadr(x);
}

#define sf_or(x, pc, ps) \
	make_sequence(EV_OR, FALSE, x, pc, ps)

cell sf_set_b(cell x, int *pc, int *ps) {
	cell	n;
	int	k;

	k = length(x);
	if (k < 3) return too_few_args(x);
	if (k > 3) return too_many_args(x);
	if (!symbol_p(cadr(x)))
		return error("set!: expected symbol, got", cadr(x));
	n = lookup(cadr(x), Environment, 1);
	if (Error_flag)
		return NIL;
	save(n);
	*pc = 2;
	*ps = EV_SET_VAL;
	return caddr(x);
}

cell find_local_variable(cell v, cell e) {
	if (e == NIL)
		return NIL;
	e = cdr(e);
	while (e != NIL) {
		if (v == caar(e))
			return car(e);
		e = cdr(e);
	}
	return NIL;
}

cell sf_define(int syntax, cell x, int *pc, int *ps) {
	cell	v, a, n, new;
	int	k;

	if (car(State_stack) == EV_ARGS)
		return error(syntax?
				"define-syntax: invalid context":
				"define: invalid context",
				x);
	k = length(x);
	if (k < 3)
		return too_few_args(x);
	if (symbol_p(cadr(x)) && k > 3)
		return too_many_args(x);
	if (!argument_list_p(cadr(x)))
		return error(syntax?
				"define-syntax: expected argument list, got":
				"define: expected argument list, got",
				cadr(x));
	if (!symbol_p(cadr(x))) {
		a = cddr(x);
		a = cons(cdadr(x), a);
		a = cons(S_lambda, a);
		save(a);
		n = caadr(x);
	}
	else {
		save(NIL);
		a = caddr(x);
		n = cadr(x);
	}
	v = find_local_variable(n, car(Environment));
	if (v == NIL) {
		new = extend(n, UNDEFINED, car(Environment));
		car(Environment) = new;
		v = cadar(Environment);
	}
	car(Stack) = binding_box(v);
	*pc = 2;
	if (syntax)
		*ps = EV_MACRO;
	else
		*ps = EV_SET_VAL;
	return a;
}

cell apply_special(cell x, int *pc, int *ps) {
	cell	sf;

	sf = car(x);
	if (sf == S_quote) return sf_quote(x);
	else if (sf == S_if) return sf_if(x, pc, ps);
	else if (sf == S_and) return sf_and(x, pc, ps);
	else if (sf == S_or) return sf_or(x, pc, ps);
	else if (sf == S_cond) return sf_cond(x, pc, ps);
	else if (sf == S_lambda) return sf_lambda(x);
	else if (sf == S_begin) return sf_begin(x, pc, ps);
	else if (sf == S_set_b) return sf_set_b(x, pc, ps);
	else if (sf == S_define) return sf_define(0, x, pc, ps);
	else if (sf == S_define_syntax) return sf_define(1, x, pc, ps);
	else fatal("internal: unknown special form");
	return UNSPECIFIC;
}

/*
 * Bignums
 */

cell make_integer(cell i) {
	cell	n;

	n = new_atom(i, NIL);
	return new_atom(T_INTEGER, n);
}

cell integer_value(char *src, cell x) {
	char	msg[100];

#ifdef REALNUM
	x = integer_argument(src, x);
	if (x == NIL)
		return 0;
#endif
	if (cddr(x) != NIL) {
		sprintf(msg, "%s: integer argument too big", src);
		error(msg, x);
		return 0;
	}
	return cadr(x);
}

cell bignum_abs(cell a) {
	cell	n;

	n = new_atom(labs(cadr(a)), cddr(a));
	return new_atom(T_INTEGER, n);
}

cell bignum_negate(cell a) {
	cell	n;

	n = new_atom(-cadr(a), cddr(a));
	return new_atom(T_INTEGER, n);
}

cell reverse_segments(cell n) {
	cell	m;

	m = NIL;
	while (n != NIL) {
		m = new_atom(car(n), m);
		n = cdr(n);
	}
	return m;
}

cell bignum_add(cell a, cell b);
cell bignum_subtract(cell a, cell b);

cell _bignum_add(cell a, cell b) {
	cell	fa, fb, result, r;
	int	carry;

	if (_bignum_negative_p(a)) {
		if (_bignum_negative_p(b)) {
			/* -A+-B --> -(|A|+|B|) */
			a = bignum_abs(a);
			save(a);
			a = bignum_add(a, bignum_abs(b));
			unsave(1);
			return bignum_negate(a);
		}
		else {
			/* -A+B --> B-|A| */
			return bignum_subtract(b, bignum_abs(a));
		}
	}
	else if (_bignum_negative_p(b)) {
		/* A+-B --> A-|B| */
		return bignum_subtract(a, bignum_abs(b));
	}
	/* A+B */
	a = reverse_segments(cdr(a));
	save(a);
	b = reverse_segments(cdr(b));
	save(b);
	carry = 0;
	result = NIL;
	save(result);
	while (a != NIL || b != NIL || carry) {
		fa = a == NIL? 0: car(a);
		fb = b == NIL? 0: car(b);
		r = fa + fb + carry;
		carry = 0;
		if (r >= INT_SEG_LIMIT) {
			r -= INT_SEG_LIMIT;
			carry = 1;
		}
		result = new_atom(r, result);
		car(Stack) = result;
		if (a != NIL) a = cdr(a);
		if (b != NIL) b = cdr(b);
	}
	unsave(3);
	return new_atom(T_INTEGER, result);
}

cell bignum_add(cell a, cell b) {
	Tmp = b;
	save(a);
	save(b);
	Tmp = NIL;
	a = _bignum_add(a, b);
	unsave(2);
	return a;
}

int bignum_less_p(cell a, cell b) {
	int	ka, kb, neg_a, neg_b;

	neg_a = _bignum_negative_p(a);
	neg_b = _bignum_negative_p(b);
	if (neg_a && !neg_b) return 1;
	if (!neg_a && neg_b) return 0;
	ka = length(a);
	kb = length(b);
	if (ka < kb) return neg_a? 0: 1;
	if (ka > kb) return neg_a? 1: 0;
	Tmp = b;
	a = bignum_abs(a);
	save(a);
	b = bignum_abs(b);
	unsave(1);
	Tmp = NIL;
	a = cdr(a);
	b = cdr(b);
	while (a != NIL) {
		if (car(a) < car(b)) return neg_a? 0: 1;
		if (car(a) > car(b)) return neg_a? 1: 0;
		a = cdr(a);
		b = cdr(b);
	}
	return 0;
}

int bignum_equal_p(cell a, cell b) {
	a = cdr(a);
	b = cdr(b);
	while (a != NIL && b != NIL) {
		if (car(a) != car(b))
			return 0;
		a = cdr(a);
		b = cdr(b);
	}
	return a == NIL && b == NIL;
}

cell _bignum_subtract(cell a, cell b) {
	cell	fa, fb, result, r;
	int	borrow;

	if (_bignum_negative_p(a)) {
		if (_bignum_negative_p(b)) {
			/* -A--B --> -A+|B| --> |B|-|A| */
			a = bignum_abs(a);
			save(a);
			a = bignum_subtract(bignum_abs(b), a);
			unsave(1);
			return a;
		}
		else {
			/* -A-B --> -(|A|+B) */
			return bignum_negate(bignum_add(bignum_abs(a), b));
		}
	}
	else if (_bignum_negative_p(b)) {
		/* A--B --> A+|B| */
		return bignum_add(a, bignum_abs(b));
	}
	/* A-B, A<B --> -(B-A) */
	if (bignum_less_p(a, b))
		return bignum_negate(bignum_subtract(b, a));
	/* A-B, A>=B */
	a = reverse_segments(cdr(a));
	save(a);
	b = reverse_segments(cdr(b));
	save(b);
	borrow = 0;
	result = NIL;
	save(result);
	while (a != NIL || b != NIL || borrow) {
		fa = a == NIL? 0: car(a);
		fb = b == NIL? 0: car(b);
		r = fa - fb - borrow;
		borrow = 0;
		if (r < 0) {
			r += INT_SEG_LIMIT;
			borrow = 1;
		}
		result = new_atom(r, result);
		car(Stack) = result;
		if (a != NIL) a = cdr(a);
		if (b != NIL) b = cdr(b);
	}
	unsave(3);
	while (car(result) == 0 && cdr(result) != NIL)
		result = cdr(result);
	return new_atom(T_INTEGER, result);
}

cell bignum_subtract(cell a, cell b) {
	Tmp = b;
	save(a);
	save(b);
	Tmp = NIL;
	a = _bignum_subtract(a, b);
	unsave(2);
	return a;
}

cell bignum_shift_left(cell a, int fill) {
	cell	r, c, result;
	int	carry;

	a = reverse_segments(cdr(a));
	save(a);
	carry = fill;
	result = NIL;
	save(result);
	while (a != NIL) {
		if (car(a) >= INT_SEG_LIMIT/10) {
			c = car(a) / (INT_SEG_LIMIT/10);
			r = car(a) % (INT_SEG_LIMIT/10) * 10;
			r += carry;
			carry = c;
		}
		else {
			r = car(a) * 10 + carry;
			carry = 0;
		}
		result = new_atom(r, result);
		car(Stack) = result;
		a = cdr(a);
	}
	if (carry)
		result = new_atom(carry, result);
	unsave(2);
	return new_atom(T_INTEGER, result);
}

/* Result: (a/10 . a%10) */
cell bignum_shift_right(cell a) {
	cell	r, c, result;
	int	carry;

	a = cdr(a);
	save(a);
	carry = 0;
	result = NIL;
	save(result);
	while (a != NIL) {
		c = car(a) % 10;
		r = car(a) / 10;
		r += carry * (INT_SEG_LIMIT/10);
		carry = c;
		result = new_atom(r, result);
		car(Stack) = result;
		a = cdr(a);
	}
	result = reverse_segments(result);
	if (car(result) == 0 && cdr(result) != NIL)
		result = cdr(result);
	result = new_atom(T_INTEGER, result);
	car(Stack) = result;
	carry = make_integer(carry);
	unsave(2);
	return cons(result, carry);
}

cell bignum_multiply(cell a, cell b) {
	int	neg;
	cell	r, i, result;

	neg = _bignum_negative_p(a) != _bignum_negative_p(b);
	a = bignum_abs(a);
	save(a);
	b = bignum_abs(b);
	save(b);
	result = make_integer(0);
	save(result);
	while (!_bignum_zero_p(a)) {
		if (Error_flag)
			break;
		r = bignum_shift_right(a);
		i = caddr(r);
		a = car(r);
		caddr(Stack) = a;
		while (i) {
			result = bignum_add(result, b);
			car(Stack) = result;
			i--;
		}
		b = bignum_shift_left(b, 0);
		cadr(Stack) = b;
	}
	if (neg)
		result = bignum_negate(result);
	unsave(3);
	return result;
}

/*
 * Equalize A and B, e.g.:
 * A=123, B=12345 ---> 12300, 100
 * Return (scaled-a . scaling-factor)
 */
cell bignum_equalize(cell a, cell b) {
	cell	r, f, r0, f0;

	r0 = a;
	save(r0);
	f0 = make_integer(1);
	save(f0);
	r = r0;
	save(r);
	f = f0;
	save(f);
	while (bignum_less_p(r, b)) {
		cadddr(Stack) = r0 = r;
		caddr(Stack) = f0 = f;
		r = bignum_shift_left(r, 0);
		cadr(Stack) = r;
		f = bignum_shift_left(f, 0);
		car(Stack) = f;
	}
	unsave(4);
	return cons(r0, f0);
}

/* Result: (a/b . a%b) */
cell _bignum_divide(cell a, cell b) {
	int	neg, neg_a;
	cell	result, f;
	int	i;
	cell	c, c0;

	neg_a = _bignum_negative_p(a);
	neg = neg_a != _bignum_negative_p(b);
	a = bignum_abs(a);
	save(a);
	b = bignum_abs(b);
	save(b);
	if (bignum_less_p(a, b)) {
		if (neg_a)
			a = bignum_negate(a);
		f = make_integer(0);
		unsave(2);
		return cons(f, a);
	}
	b = bignum_equalize(b, a);
	cadr(Stack) = b; /* cadr+cddddr */
	car(Stack) = a;	/* car+cddddr */
	c = NIL;
	save(c);	/* cadddr */
	c0 = NIL;
	save(c0);	/* caddr */
	f = cdr(b);
	b = car(b);
	cadddr(Stack) = b;
	save(f);	/* cadr */
	result = make_integer(0);
	save(result);	/* car */
	while (!_bignum_zero_p(f)) {
		if (Error_flag)
			break;
		c = make_integer(0);
		cadddr(Stack) = c;
		caddr(Stack) = c0 = c;
		i = 0;
		while (!bignum_less_p(a, c)) {
			if (Error_flag)
				break;
			caddr(Stack) = c0 = c;
			c = bignum_add(c, b);
			cadddr(Stack) = c;
			i++;
		}
		result = bignum_shift_left(result, i-1);
		car(Stack) = result;
		a = bignum_subtract(a, c0);
		car(cddddr(Stack)) = a;
		f = car(bignum_shift_right(f));
		cadr(Stack) = f;
		b = car(bignum_shift_right(b));
		cadr(cddddr(Stack)) = b;
	}
	if (neg)
		result = bignum_negate(result);
	car(Stack) = result;
	if (neg_a)
		a = bignum_negate(a);
	unsave(6);
	return cons(result, a);
}

cell bignum_divide(cell x, cell a, cell b) {
	if (_bignum_zero_p(b))
		return error("divide by zero", x);
	Tmp = b;
	save(a);
	save(b);
	Tmp = NIL;
	a = _bignum_divide(a, b);
	unsave(2);
	return a;
}

cell bignum_read(char *pre, int radix) {
	char	digits[] = "0123456789abcdef";
	char	buf[100];
	cell	base, num;
	int	c, s, p, nd;

	base = make_integer(radix);
	save(base);
	num = make_integer(0);
	save(num);
	c = read_c_ci();
	s = 0;
	if (c == '-') {
		s = 1;
		c = read_c_ci();
	}
	else if (c == '+') {
		c = read_c_ci();
	}
	nd = 0;
	while (!separator(c)) {
		p = 0;
		while (digits[p] && digits[p] != c)
			p++;
		if (p >= radix) {
			sprintf(buf, "invalid digit in %s number", pre);
			unsave(2);
			return funny_char(buf, c);
		}
		num = bignum_multiply(num, base);
		car(Stack) = num;
		num = bignum_add(num, make_integer(p));
		car(Stack) = num;
		nd++;
		c = read_c_ci();
	}
	unsave(2);
	if (!nd) {
		sprintf(buf, "digits expected after %s", pre);
		return error(buf, NOEXPR);
	}
	reject(c);
	return s? bignum_negate(num): num;
}

/*
 * Primitives
 */

cell pp_apply(cell x) {
	cell	m, p, q, last;

	m = cdr(x);
	p = cdr(m);
	last = p;
	while (p != NIL) {
		last = p;
		p = cdr(p);
	}
	p = car(last);
	while (p != NIL) {
		if (atom_p(p))
			return error("apply: improper argument list",
					car(last));
		p = cdr(p);
	}
	if (cddr(m) == NIL) {
		p = cadr(m);
	}
	else {
		p = flat_copy(cdr(m), &q);
		q = p;
		while (cddr(q) != NIL)
			q = cdr(q);
		cdr(q) = car(last);
	}
	return cons(car(m), p);
}

cell pp_call_cc(cell x) {
	cell	cc, n;

	cc = cons(Stack, NIL);
	cc = cons(Stack_bottom, cc);
	cc = cons(State_stack, cc);
	cc = cons(Environment, cc);
	cc = new_atom(T_CONTINUATION, cc);
	n = cons(cc, NIL);
	n = cons(cadr(x), n);
	return n;
}

cell resume(cell x) {
	cell	cc;

	if (cdr(x) == NIL) return too_few_args(x);
	if (cddr(x) != NIL) return too_many_args(x);
	cc = cdar(x);
	Environment = car(cc);
	State_stack = cadr(cc);
	Stack_bottom = caddr(cc);
	Stack = cadddr(cc);
	return cadr(x);
}

cell pp_unquote(cell x) {
	return error("unquote: not in quasiquote context", NOEXPR);
}

cell pp_unquote_splicing(cell x) {
	return error("unquote-splicing: not in quasiquote context", NOEXPR);
}

/*
 * Predicates and Booleans
 */

cell pp_eq_p(cell x) {
	return cadr(x) == caddr(x)? TRUE: FALSE;
}

int eqv_p(cell a, cell b) {
	if (a == b)
		return 1;
	if (char_p(a) && char_p(b) && char_value(a) == char_value(b))
		return 1;
	if (number_p(a) && number_p(b)) {
		if (real_p(a) != real_p(b))
			return 0;
		return real_equal_p(a, b);
	}
	return a == b;
}

cell pp_eqv_p(cell x) {
	return eqv_p(cadr(x), caddr(x))? TRUE: FALSE;
}

cell pp_not(cell x) {
	return cadr(x) == FALSE? TRUE: FALSE;
}

cell pp_null_p(cell x) {
	return cadr(x) == NIL? TRUE: FALSE;
}

/*
 * Pairs and Lists
 */

cell pp_append2(cell x) {
	cell	new, n, p, a, *pa;

	if (cadr(x) == NIL) return caddr(x);
	if (caddr(x) == NIL) {
		if (pair_p(cadr(x)))
			return cadr(x);
		else
			return error("append2: expected list, got",
					cadr(x));
	}
	a = n = cons(NIL, NIL);
	pa = &a;
	save(n);
	for (p = cadr(x); p != NIL; p = cdr(p)) {
		if (!pair_p(p))
			return error("append2: improper list", cadr(x));
		car(a) = car(p);
		new = cons(NIL, NIL);
		cdr(a) = new;
		pa = &cdr(a);
		a = cdr(a);
	}
	unsave(1);
	*pa = caddr(x);
	return n;
}

int assqv(char *who, int v, cell x, cell a) {
	cell	p;
	char	buf[64];

	for (p = a; p != NIL; p = cdr(p)) {
		if (!pair_p(p) || !pair_p(car(p))) {
			sprintf(buf, "%s: bad element in alist", who);
			return error(buf, p);
		}
		if (!v && x == caar(p))
			return car(p);
		if (v && eqv_p(x, caar(p)))
			return car(p);
	}
	return FALSE;
}

cell pp_assq(cell x) {
	return assqv("assq", 0, cadr(x), caddr(x));
}

cell pp_assv(cell x) {
	return assqv("assv", 1, cadr(x), caddr(x));
}

char *rev_cxr_name(char *s) {
	int		i, k = strlen(s);
	static char	buf[8];

	for (i=0; i<k; i++) {
		buf[i] = s[k-i-1];
	}
	buf[i] = 0;
	return buf;
}

cell cxr(char *op, cell x) {
	char	*p;
	cell	n = cadr(x);
	char	buf[64];

	for (p = &op[1]; *p != 'c'; p++) {
		if (!pair_p(n)) {
			sprintf(buf, "%s: unsuitable type for operation",
				rev_cxr_name(op));
			error(buf, cadr(x));
		}
		if (*p == 'a')
			n = car(n);
		else 
			n = cdr(n);
	}
	return n;
}

cell pp_caar(cell x)   { return cxr("raac", x); }
cell pp_cadr(cell x)   { return cxr("rdac", x); }
cell pp_cdar(cell x)   { return cxr("radc", x); }
cell pp_cddr(cell x)   { return cxr("rddc", x); }
cell pp_caaar(cell x)  { return cxr("raaac", x); }
cell pp_caadr(cell x)  { return cxr("rdaac", x); }
cell pp_cadar(cell x)  { return cxr("radac", x); }
cell pp_caddr(cell x)  { return cxr("rddac", x); }
cell pp_cdaar(cell x)  { return cxr("raadc", x); }
cell pp_cdadr(cell x)  { return cxr("rdadc", x); }
cell pp_cddar(cell x)  { return cxr("raddc", x); }
cell pp_cdddr(cell x)  { return cxr("rdddc", x); }
cell pp_caaaar(cell x) { return cxr("raaaac", x); }
cell pp_caaadr(cell x) { return cxr("rdaaac", x); }
cell pp_caadar(cell x) { return cxr("radaac", x); }
cell pp_caaddr(cell x) { return cxr("rddaac", x); }
cell pp_cadaar(cell x) { return cxr("raadac", x); }
cell pp_cadadr(cell x) { return cxr("rdadac", x); }
cell pp_caddar(cell x) { return cxr("raddac", x); }
cell pp_cadddr(cell x) { return cxr("rdddac", x); }
cell pp_cdaaar(cell x) { return cxr("raaadc", x); }
cell pp_cdaadr(cell x) { return cxr("rdaadc", x); }
cell pp_cdadar(cell x) { return cxr("radadc", x); }
cell pp_cdaddr(cell x) { return cxr("rddadc", x); }
cell pp_cddaar(cell x) { return cxr("raaddc", x); }
cell pp_cddadr(cell x) { return cxr("rdaddc", x); }
cell pp_cdddar(cell x) { return cxr("radddc", x); }
cell pp_cddddr(cell x) { return cxr("rddddc", x); }

cell pp_car(cell x) {
	return caadr(x);
}

cell pp_cdr(cell x) {
	return cdadr(x);
}

cell pp_cons(cell x) {
	return cons(cadr(x), caddr(x));
}

cell pp_length(cell x) {
	int	k, c1;

	c1 = make_integer(1);
	save(c1);
	k = make_integer(0);
	save(k);
	for (x = cadr(x); x != NIL; x = cdr(x)) {
		if (!pair_p(x)) {
			unsave(2);
			return error("length: improper list", x);
		}
		k = bignum_add(k, c1);
		car(Stack) = k;
	}
	unsave(2);
	return k;
}

cell pp_list(cell x) {
	return cdr(x);
}

cell pp_list_tail(cell x) {
	cell	c1, p, k = caddr(x);

	c1 = make_integer(1);
	save(c1);
	save(k);
	for (p = cadr(x); p != NIL; p = cdr(p)) {
		if (!pair_p(p)) {
			unsave(2);
			return error("list-tail: improper list", p);
		}
		if (_bignum_zero_p(k))
			break;
		k = bignum_subtract(k, c1);
		car(Stack) = k;
	}
	unsave(2);
	if (!_bignum_zero_p(k))
		return error("list-tail: index out of range", cadr(x));
	return p;
}

int memqv(char *who, int v, cell x, cell a) {
	cell	p;
	char	buf[64];

	for (p = a; p != NIL; p = cdr(p)) {
		if (!pair_p(p)) {
			sprintf(buf, "%s: improper list", who);
			return error(buf, p);
		}
		if (!v && x == car(p))
			return p;
		if (v && eqv_p(x, car(p)))
			return p;
	}
	return FALSE;
}

cell pp_memq(cell x) {
	return memqv("memq", 0, cadr(x), caddr(x));
}

cell pp_memv(cell x) {
	return memqv("memv", 1, cadr(x), caddr(x));
}

cell pp_reverse(cell x) {
	cell	n, m;

	m = NIL;
	for (n = cadr(x); n != NIL; n = cdr(n)) {
		if (!pair_p(n))
			return error("reverse: expected list, got", cadr(x));
		m = cons(car(n), m);
	}
	return m;
}

cell pp_set_car_b(cell x) {
	if (constant_p(cadr(x)))
		return error("set-car!: immutable object", cadr(x));
	caadr(x) = caddr(x);
	return UNSPECIFIC;
}

cell pp_set_cdr_b(cell x) {
	if (constant_p(cadr(x)))
		return error("set-cdr!: immutable object", cadr(x));
	cdadr(x) = caddr(x);
	return UNSPECIFIC;
}

/*
 * Arithmetics
 */

cell pp_abs(cell x) {
	return real_abs(cadr(x));
}

cell pp_equal(cell x) {
	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!number_p(cadr(x)))
			return error("=: expected number, got", cadr(x));
		if (!real_equal_p(car(x), cadr(x)))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

int even_p(char *who, cell x) {
#ifdef REALNUM
	x = integer_argument(who, x);
	if (x == NIL)
		return UNDEFINED;
	save(x);
	x = reverse_segments(cdr(x));
	unsave(1);
#else
	x = reverse_segments(x);
#endif
	return car(x) % 2 == 0;
}

cell pp_even_p(cell x) {
	return even_p("even?", cadr(x))? TRUE: FALSE;
}

cell pp_greater(cell x) {
	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!number_p(cadr(x)))
			return error(">: expected number, got", cadr(x));
		if (!real_less_p(cadr(x), car(x)))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

cell pp_greater_equal(cell x) {
	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!number_p(cadr(x)))
			return error(">=: expected number, got", cadr(x));
		if (real_less_p(car(x), cadr(x)))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

cell pp_less(cell x) {
	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!number_p(cadr(x)))
			return error("<: expected number, got", cadr(x));
		if (!real_less_p(car(x), cadr(x)))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

cell pp_less_equal(cell x) {
	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!number_p(cadr(x)))
			return error("<=: expected number, got", cadr(x));
		if (real_less_p(cadr(x), car(x)))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

cell limit(char *msg, int(*pred)(cell,cell), cell x) {
	cell	k, p;
	int	exact = 1;

	k = cadr(x);
	if (real_p(k))
		exact = 0;
	for (p = cddr(x); p != NIL; p = cdr(p)) {
		if (!number_p(car(p)))
			return error(msg, (car(p)));
		if (real_p(car(p)))
			exact = 0;
		if (pred(k, car(p)))
			k = car(p);
	}
	if (exact)
		return k;
	if (integer_p(k))
		return bignum_to_real(k);
	return k;
}

cell pp_max(cell x) {
	return limit("max: expected number, got", real_less_p, x);
}

cell pp_minus(cell x) {
	cell	a;

	x = cdr(x);
	if (cdr(x) == NIL)
		return real_negate(car(x));
	a = car(x);
	x = cdr(x);
	save(a);
	while (x != NIL) {
		if (!number_p(car(x))) {
			unsave(1);
			return error("-: expected number, got", car(x));
		}
		a = real_subtract(a, car(x));
		car(Stack) = a;
		x = cdr(x);
	}
	unsave(1);
	return a;
}

int real_greater_p(cell x, cell y) {
	return real_less_p(y, x);
}

cell pp_min(cell x) {
	return limit("max: expected number, got", real_greater_p, x);
}

cell pp_negative_p(cell x) {
	return real_negative_p(cadr(x))? TRUE: FALSE;
}

cell pp_odd_p(cell x) {
	return even_p("odd?", cadr(x))? FALSE: TRUE;
}

cell pp_plus(cell x) {
	cell	a;

	x = cdr(x);
	if (x == NIL)
		return make_integer(0);
	if (cdr(x) == NIL)
		return car(x);
	a = make_integer(0);
	save(a);
	while (x != NIL) {
		if (!number_p(car(x))) {
			unsave(1);
			return error("+: expected number, got", car(x));
		}
		a = real_add(a, car(x));
		car(Stack) = a;
		x = cdr(x);
	}
	unsave(1);
	return a;
}

cell pp_quotient(cell x) {
        char    *name = "quotient";
        cell    a, b;

	name = name; /*LINT*/
        a = integer_argument(name, cadr(x));
        save(a);
        b = integer_argument(name, caddr(x));
        unsave(1);
	if (a == NIL || b == NIL)
		return UNDEFINED;
        return car(bignum_divide(x, a, b));
}

cell pp_positive_p(cell x) {
	x = cadr(x);
	return real_positive_p(x)? TRUE: FALSE;
}

cell pp_remainder(cell x) {
        char    *name = "remainder";
        cell    a, b;

	name = name; /*LINT*/
        a = integer_argument(name, cadr(x));
        save(a);
        b = integer_argument(name, caddr(x));
        unsave(1);
	if (a == NIL || b == NIL)
		return UNDEFINED;
        return cdr(bignum_divide(x, a, b));
}

cell pp_times(cell x) {
	cell	a;

	x = cdr(x);
	if (x == NIL)
		return make_integer(1);
	if (cdr(x) == NIL)
		return car(x);
	a = make_integer(1);
	save(a);
	while (x != NIL) {
		if (!number_p(car(x))) {
			unsave(1);
			return error("*: expected number, got", car(x));
		}
		a = real_multiply(a, car(x));
		car(Stack) = a;
		x = cdr(x);
	}
	unsave(1);
	return a;
}

cell pp_zero_p(cell x) {
	return real_zero_p(cadr(x))? TRUE: FALSE;
}

/*
 * Type Predicates and Conversion
 */

cell pp_boolean_p(cell x) {
	return boolean_p(cadr(x))? TRUE: FALSE;
}

cell pp_char_p(cell x) {
	return char_p(cadr(x))? TRUE: FALSE;
}

cell pp_char_to_integer(cell x) {
	return make_integer(char_value(cadr(x)));
}

cell pp_input_port_p(cell x) {
	return input_port_p(cadr(x))? TRUE: FALSE;
}

cell pp_integer_to_char(cell x) {
	cell	n;

	n = integer_value("integer->char", cadr(x));
	if (n < 0 || n > 255)
		return error("integer->char: argument value out of range",
				cadr(x));
	return make_char(n);
}

cell pp_integer_p(cell x) {
	return real_integer_p(cadr(x))? TRUE: FALSE;
}

cell list_to_string(char *who, cell x) {
	cell	n;
	int	k = length(x);
	char	*s;
	char	buf[100];

	n = make_string("", k);
	s = string(n);
	while (x != NIL) {
		if (atom_p(x))
			return error("list->string: improper list", x);
		if (!char_p(car(x))) {
			sprintf(buf, "%s: expected list of char,"
					" got list containing",
				who);
			return error(buf, car(x));
		}
		*s++ = cadar(x);
		x = cdr(x);
	}
	*s = 0;
	return n;
}

cell pp_list_to_string(cell x) {
	return list_to_string("list->string", cadr(x));
}

cell pp_list_to_vector(cell x) {
	return list_to_vector(cadr(x), "list->vector: improper list", 0);
}

cell pp_output_port_p(cell x) {
	return output_port_p(cadr(x))? TRUE: FALSE;
}

cell pp_pair_p(cell x) {
	return pair_p(cadr(x))? TRUE: FALSE;
}

cell pp_procedure_p(cell x) {
	return (procedure_p(cadr(x)) ||
		primitive_p(cadr(x)) ||
		continuation_p(cadr(x)))?
			TRUE: FALSE;
}

cell pp_string_to_list(cell x) {
	char	*s;
	cell	n, a, new;
	int	k, i;

	k = string_len(cadr(x));
	n = NIL;
	a = NIL;
	for (i=0; i<k-1; i++) {
		s = string(cadr(x));
		if (n == NIL) {
			n = a = cons(make_char(s[i]), NIL);
			save(n);
		}
		else {
			new = cons(make_char(s[i]), NIL);
			cdr(a) = new;
			a = cdr(a);
		}
	}
	if (n != NIL)
		unsave(1);
	return n;
}

cell pp_string_to_symbol(cell x) {
	cell	y, n;

	y = find_symbol(string(cadr(x)));
	if (y != NIL)
		return y;
	/*
	 * Cannot pass name to make_symbol(), because
	 * string(cadr(x)) may move during GC.
	 */
	n = make_symbol("", string_len(cadr(x))-1);
	strcpy(symbol_name(n), string(cadr(x)));
	Symbols = cons(n, Symbols);
	return car(Symbols);
}

cell pp_string_p(cell x) {
	return string_p(cadr(x))? TRUE: FALSE;
}

cell pp_symbol_to_string(cell x) {
	cell	n;

	/*
	 * Cannot pass name to make_string(), because
	 * symbol_name(cadr(x)) may move during GC.
	 */
	n = make_string("", symbol_len(cadr(x))-1);
	Tag[n] |= CONST_TAG;
	strcpy(string(n), symbol_name(cadr(x)));
	return n;
}

cell pp_symbol_p(cell x) {
	return symbol_p(cadr(x))? TRUE: FALSE;
}

cell pp_vector_to_list(cell x) {
	cell	*v;
	cell	n, a, new;
	int	k, i;

	k = vector_len(cadr(x));
	n = NIL;
	a = NIL;
	for (i=0; i<k; i++) {
		v = vector(cadr(x));
		if (n == NIL) {
			n = a = cons(v[i], NIL);
			save(n);
		}
		else {
			new = cons(v[i], NIL);
			cdr(a) = new;
			a = cdr(a);
		}
	}
	if (n != NIL)
		unsave(1);
	return n;
}

cell pp_vector_p(cell x) {
	return vector_p(cadr(x))? TRUE: FALSE;
}

/*
 * Characters
 */

cell pp_char_alphabetic_p(cell x) {
	return isalpha(char_value(cadr(x)))? TRUE: FALSE;
}

#define L(c) tolower(c)
int char_ci_le(int c1, int c2) { return L(c1) <= L(c2); }
int char_ci_lt(int c1, int c2) { return L(c1) <  L(c2); }
int char_ci_eq(int c1, int c2) { return L(c1) == L(c2); }
int char_ci_ge(int c1, int c2) { return L(c1) >= L(c2); }
int char_ci_gt(int c1, int c2) { return L(c1) >  L(c2); }

int char_le(int c1, int c2) { return c1 <= c2; }
int char_lt(int c1, int c2) { return c1 <  c2; }
int char_eq(int c1, int c2) { return c1 == c2; }
int char_ge(int c1, int c2) { return c1 >= c2; }
int char_gt(int c1, int c2) { return c1 >  c2; }

cell char_predicate(char *name, int (*p)(int c1, int c2), cell x) {
	char	msg[100];

	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!char_p(cadr(x))) {
			sprintf(msg, "%s: expected char, got", name);
			return error(msg, cadr(x));
		}
		if (!p(char_value(car(x)), char_value(cadr(x))))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

#define CP return char_predicate
cell pp_char_ci_le_p(cell x) { CP("char-ci<=?", char_ci_le, x); }
cell pp_char_ci_lt_p(cell x) { CP("char-ci<?",  char_ci_lt, x); }
cell pp_char_ci_eq_p(cell x) { CP("char-ci=?",  char_ci_eq, x); }
cell pp_char_ci_ge_p(cell x) { CP("char-ci>=?", char_ci_ge, x); }
cell pp_char_ci_gt_p(cell x) { CP("char-ci>?",  char_ci_gt, x); }

cell pp_char_le_p(cell x) { CP("char<=?", char_le, x); }
cell pp_char_lt_p(cell x) { CP("char<?",  char_lt, x); }
cell pp_char_eq_p(cell x) { CP("char=?",  char_eq, x); }
cell pp_char_ge_p(cell x) { CP("char>=?", char_ge, x); }
cell pp_char_gt_p(cell x) { CP("char>?",  char_gt, x); }

cell pp_char_downcase(cell x) {
	return make_char(tolower(char_value(cadr(x))));
}

cell pp_char_lower_case_p(cell x) {
	return islower(char_value(cadr(x)))? TRUE: FALSE;
}

cell pp_char_numeric_p(cell x) {
	return isdigit(char_value(cadr(x)))? TRUE: FALSE;
}

cell pp_char_upcase(cell x) {
	return make_char(toupper(char_value(cadr(x))));
}

cell pp_char_upper_case_p(cell x) {
	return isupper(char_value(cadr(x)))? TRUE: FALSE;
}

cell pp_char_whitespace_p(cell x) {
	int	c = char_value(cadr(x));

	return (c == ' '  || c == '\t' || c == '\n' ||
		c == '\r' || c == '\f')? TRUE: FALSE;
}

/*
 * Strings
 */

cell pp_make_string(cell x) {
	cell	n;
	int	c, k;
	char	*s;

	k = integer_value("make-string", cadr(x));
	if (k < 0)
		return error("make-string: got negative length", x);
	n = make_string("", k);
	s = string(n);
	c = cddr(x) == NIL? ' ': char_value(caddr(x));
	memset(s, c, k);
	s[k] = 0;
	return n;
}

cell pp_string(cell x) {
	return list_to_string("string", cdr(x));
}

cell pp_string_append(cell x) {
	cell	p, n;
	int	k;
	char	*s;

	k = 0;
	for (p = cdr(x); p != NIL; p = cdr(p)) {
		if (!string_p(car(p)))
			return error("string-append: expected string, got",
					car(p));
		k += string_len(car(p))-1;
	}
	n = make_string("", k);
	s = string(n);
	k = 0;
	for (p = cdr(x); p != NIL; p = cdr(p)) {
		strcpy(&s[k], string(car(p)));
		k += string_len(car(p))-1;
	}
	return n;
}

cell pp_string_copy(cell x) {
	cell	n;

	/*
	 * Cannot pass name to make_string(), because
	 * string(cadr(x)) may move during GC.
	 */
	n = make_string("", string_len(cadr(x))-1);
	strcpy(string(n), string(cadr(x)));
	return n;
}

cell pp_string_fill_b(cell x) {
	int	c = char_value(caddr(x)),
		i, k = string_len(cadr(x))-1;
	char	*s = string(cadr(x));

	if (constant_p(cadr(x)))
		return error("string-fill!: immutable object", cadr(x));
	for (i=0; i<k; i++)
		s[i] = c;
	return UNSPECIFIC;
}

cell pp_string_length(cell x) {
	return make_integer(string_len(cadr(x))-1);
}

cell pp_string_ref(cell x) {
	int	p, k = string_len(cadr(x))-1;

	p = integer_value("string-ref", caddr(x));
	if (p < 0 || p >= k)
		return error("string-ref: index out of range",
				caddr(x));
	return make_char(string(cadr(x))[p]);
}

cell pp_string_set_b(cell x) {
	int	p, k = string_len(cadr(x))-1;

	if (constant_p(cadr(x)))
		return error("string-set!: immutable object", cadr(x));
	p = integer_value("string-set!", caddr(x));
	if (p < 0 || p >= k)
		return error("string-set!: index out of range",
				caddr(x));
	string(cadr(x))[p] = char_value(cadddr(x));
	return UNSPECIFIC;
}

int string_ci_le(char *s1, char *s2) { return strcmp_ci(s1, s2) <= 0; }
int string_ci_lt(char *s1, char *s2) { return strcmp_ci(s1, s2) <  0; }
int string_ci_eq(char *s1, char *s2) { return strcmp_ci(s1, s2) == 0; }
int string_ci_ge(char *s1, char *s2) { return strcmp_ci(s1, s2) >= 0; }
int string_ci_gt(char *s1, char *s2) { return strcmp_ci(s1, s2) >  0; }

int string_le(char *s1, char *s2) { return strcmp(s1, s2) <= 0; }
int string_lt(char *s1, char *s2) { return strcmp(s1, s2) <  0; }
int string_eq(char *s1, char *s2) { return strcmp(s1, s2) == 0; }
int string_ge(char *s1, char *s2) { return strcmp(s1, s2) >= 0; }
int string_gt(char *s1, char *s2) { return strcmp(s1, s2) >  0; }

cell string_predicate(char *name, int (*p)(char *s1, char *s2), cell x) {
	char	msg[100];

	x = cdr(x);
	while (cdr(x) != NIL) {
		if (!string_p(cadr(x))) {
			sprintf(msg, "%s: expected string, got", name);
			return error(msg, cadr(x));
		}
		if (	(p == string_eq || p == string_ci_eq) &&
			string_len(car(x)) != string_len(cadr(x))
		)
			return FALSE;
		if (!p(string(car(x)), string(cadr(x))))
			return FALSE;
		x = cdr(x);
	}
	return TRUE;
}

#define SP return string_predicate
cell pp_string_ci_le_p(cell x) { SP("string-ci<=?", string_ci_le, x); }
cell pp_string_ci_lt_p(cell x) { SP("string-ci<?",  string_ci_lt, x); }
cell pp_string_ci_eq_p(cell x) { SP("string-ci=?",  string_ci_eq, x); }
cell pp_string_ci_ge_p(cell x) { SP("string-ci>=?", string_ci_ge, x); }
cell pp_string_ci_gt_p(cell x) { SP("string-ci>?",  string_ci_gt, x); }

cell pp_string_le_p(cell x) { SP("string<=?", string_le, x); }
cell pp_string_lt_p(cell x) { SP("string<?",  string_lt, x); }
cell pp_string_eq_p(cell x) { SP("string=?",  string_eq, x); }
cell pp_string_ge_p(cell x) { SP("string>=?", string_ge, x); }
cell pp_string_gt_p(cell x) { SP("string>?",  string_gt, x); }

cell pp_substring(cell x) {
	int	k = string_len(cadr(x))-1;
	int	p0 = integer_value("substring", caddr(x));
	int	pn = integer_value("substring", cadddr(x));
	char	*src, *dst;
	cell	n;

	if (p0 < 0 || p0 > k || pn < 0 || pn > k || pn < p0) {
		n = cons(cadddr(x), NIL);
		return error("substring: invalid range",
				cons(caddr(x), n));
	}
	n = make_string("", pn-p0);
	dst = string(n);
	src = string(cadr(x));
	if (pn-p0 != 0)
		memcpy(dst, &src[p0], pn-p0);
	dst[pn-p0] = 0;
	return n;
}

/*
 * Vectors
 */

cell pp_make_vector(cell x) {
	int	i, k;
	cell	n, *v, m;

	k = integer_value("make-vector", cadr(x));
	if (k < 0)
		return error("make-vector: got negative length", x);
	n = new_vec(T_VECTOR, k * sizeof(cell));
	v = vector(n);
	m = cddr(x) == NIL? FALSE: caddr(x);
	for (i=0; i<k; i++)
		v[i] = m;
	return n;
}

cell pp_vector(cell x) {
	return list_to_vector(cdr(x), "vector: improper list", 0);
}

cell pp_vector_append(cell x) {
	cell	n, p, *ov, *nv;
	int	i, j, k, total;

	total = 0;
	for (p = cdr(x); p != NIL; p = cdr(p))
		if (vector_p(car(p)))
			total += vector_len(car(p));
		else
			return error("vector-append: expected vector, got",
					car(p));
	n = new_vec(T_VECTOR, total * sizeof(cell));;
	nv = vector(n);
	j = 0;
	for (p = cdr(x); p != NIL; p = cdr(p)) {
		ov = vector(car(p));
		k = vector_len(car(p));
		for (i = 0; i < k; i++)
			nv[j++] = ov[i];
	}
	return n;
}

cell pp_vector_copy(cell x) {
	cell	n, vec, *ov, *nv;
	int	k0 = 0, kn, k = vector_len(cadr(x));
	int	i, j;
	cell	fill = UNSPECIFIC;
	char	err[] = "vector-copy: expected integer, got";
	char	name[] = "vector-copy";

	kn = k;
	vec = cadr(x);
	x = cddr(x);
	if (x != NIL) {
		if (!number_p(car(x))) return error(err, car(x));
		k0 = integer_value(name, car(x));
		x = cdr(x);
	}
	if (x != NIL) {
		if (!number_p(car(x))) return error(err, car(x));
		kn = integer_value(name, car(x));
		x = cdr(x);
	}
	if (k0 > kn)
		return error("vector-copy: bad range", NOEXPR);
	if (x != NIL) {
		fill = car(x);
		x = cdr(x);
	}
	if (x != NIL)
		return error("vector-copy: too many arguments", NOEXPR);
	n = new_vec(T_VECTOR, (kn-k0) * sizeof(cell));
	nv = vector(n);
	ov = vector(vec);
	for (j = 0, i = k0; i < kn; i++, j++)
		if (i >= k)
			nv[j] = fill;
		else
			nv[j] = ov[i];
	return n;
}

cell pp_vector_fill_b(cell x) {
	cell	fill = caddr(x);
	int	i, k = vector_len(cadr(x));
	cell	*v = vector(cadr(x));

	if (constant_p(cadr(x)))
		return error("vector-fill!: immutable object", cadr(x));
	for (i=0; i<k; i++)
		v[i] = fill;
	return UNSPECIFIC;
}

cell pp_vector_length(cell x) {
	return make_integer(vector_len(cadr(x)));
}

cell pp_vector_ref(cell x) {
	int	p, k = vector_len(cadr(x));

	p = integer_value("vector-ref", caddr(x));
	if (p < 0 || p >= k)
		return error("vector-ref: index out of range",
				caddr(x));
	return vector(cadr(x))[p];
}

cell pp_vector_set_b(cell x) {
	int	p, k = vector_len(cadr(x));

	if (constant_p(cadr(x)))
		return error("vector-set!: immutable object", cadr(x));
	p = integer_value("vector-set!", caddr(x));
	if (p < 0 || p >= k)
		return error("vector-set!: index out of range",
				caddr(x));
	vector(cadr(x))[p] = cadddr(x);
	return UNSPECIFIC;
}

/*
 * I/O
 */

void close_port(int port) {
	if (port < 0 || port >= MAX_PORTS)
		return;
	if (Ports[port] == NULL) {
		Port_flags[port] = 0;
		return;
	}
	fclose(Ports[port]); /* already closed? don't care */
	Ports[port] = NULL;
	Port_flags[port] = 0;
}

cell pp_close_input_port(cell x) {
	if (port_no(cadr(x)) < 2)
		return error("please do not close the standard input port",
				NOEXPR);
	close_port(port_no(cadr(x)));
	return UNSPECIFIC;
}

cell pp_close_output_port(cell x) {
	if (port_no(cadr(x)) < 2)
		return error("please do not close the standard output port",
				NOEXPR);
	close_port(port_no(cadr(x)));
	return UNSPECIFIC;
}

cell make_port(int portno, cell type) {
	cell	n;
	int	pf;

	pf = Port_flags[portno];
	Port_flags[portno] |= LOCK_TAG;
	n = new_atom(portno, NIL);
	n = cons3(type, n, ATOM_TAG|PORT_TAG);
	Port_flags[portno] = pf;
	return n;
}

cell pp_current_input_port(cell x) {
	return make_port(Input_port, T_INPUT_PORT);
}

cell pp_current_output_port(cell x) {
	return make_port(Output_port, T_OUTPUT_PORT);
}

cell pp_write(cell x);

cell pp_display(cell x) {
	Displaying = 1;
	pp_write(x);
	Displaying = 0;
	return UNSPECIFIC;
}

cell pp_eof_object_p(cell x) {
	return cadr(x) == END_OF_FILE? TRUE: FALSE;
}

int new_port(void) {
	int	i, tries;

	for (tries=0; tries<2; tries++) {
		for (i=0; i<MAX_PORTS; i++) {
			if (Ports[i] == NULL) {
				return i;
			}
		}
		if (tries == 0)
			gc();
	}
	return -1;
}

int open_port(char *path, char *mode) {
	int	i = new_port();

	if (i < 0)
		return -1;
	Ports[i] = fopen(path, mode);
	if (Ports[i] == NULL)
		return -1;
	else
		return i;
}

cell eval(cell x);

int load(char *file) {
	int	n;
	int	outer_lno;
	int	new_port, old_port;
	int	outer_loading;

	new_port = open_port(file, "r");
	if (new_port == -1)
		return -1;
	Port_flags[new_port] |= LOCK_TAG;
	File_list = cons(make_string(file, (int) strlen(file)), File_list);
	save(Environment);
	while (cdr(Environment) != NIL)
		Environment = cdr(Environment);
	outer_loading = box_value(S_loading);
	box_value(S_loading) = TRUE;
	old_port = Input_port;
	outer_lno = Line_no;
	Line_no = 1;
	while (!Error_flag) {
		Input_port = new_port;
		n = xread();
		Input_port = old_port;
		if (n == END_OF_FILE)
			break;
		if (!Error_flag)
			n = eval(n);
	}
	close_port(new_port);
	Line_no = outer_lno;
	box_value(S_loading) = outer_loading;
	File_list = cdr(File_list);
	rehash(car(Environment));
	Environment = unsave(1);
	return 0;
}

cell pp_load(cell x) {
	char	file[TOKEN_LENGTH+1];

	if (string_len(cadr(x)) > TOKEN_LENGTH)
		return error("load: path too long", cadr(x));
	strcpy(file, string(cadr(x)));
	if (load(file) < 0)
		return error("load: cannot open file", cadr(x));
	return UNSPECIFIC;
}

cell pp_open_input_file(cell x) {
	int	p;

	p = open_port(string(cadr(x)), "r");
	if (p < 0)
		return error("open-input-file: could not open file",
				cadr(x));
	return make_port(p, T_INPUT_PORT);
}

cell pp_open_output_file(cell x) {
	int	p;
	FILE	*f;

	f = fopen(string(cadr(x)), "r");
	if (f != NULL) {
		fclose(f);
		return error("open-output-file: file already exists",
				cadr(x));
	}
	p = open_port(string(cadr(x)), "w");
	if (p < 0)
		return error("open-output-file: could not open file",
				cadr(x));
	return make_port(p, T_OUTPUT_PORT);
}

cell pp_read(cell x) {
	cell	n;
	int	new_port, old_port;

	new_port = cdr(x) == NIL? Input_port: port_no(cadr(x));
	if (new_port < 0 || new_port >= MAX_PORTS)
		return error("read: invalid input port (oops)", cadr(x));
	old_port = Input_port;
	Input_port = new_port;
	n = xread();
	Input_port = old_port;
	return n;
}

cell read_char(cell x, int unget) {
	int	c, new_port, old_port;

	new_port = cdr(x) == NIL? Input_port: port_no(cadr(x));
	if (new_port < 0 || new_port >= MAX_PORTS)
		return error("read-char: invalid input port (oops)", cadr(x));
	if (Ports[new_port] == NULL)
		return error("read-char: input port is not open", NOEXPR);
	old_port = Input_port;
	Input_port = new_port;
	c = read_c();
	if (unget)
		reject(c);
	Input_port = old_port;
	return c == EOF? END_OF_FILE: make_char(c);
}

cell pp_peek_char(cell x) {
	return read_char(x, 1);
}

cell pp_read_char(cell x) {
	return read_char(x, 0);
}

cell pp_write(cell x) {
	int	new_port, old_port;

	new_port = cddr(x) == NIL? Output_port: port_no(caddr(x));
	if (new_port < 0 || new_port >= MAX_PORTS)
		return error("write: invalid output port (oops)", caddr(x));
	old_port = Output_port;
	Output_port = new_port;
	print_form(cadr(x));
	Output_port = old_port;
	return UNSPECIFIC;
}

cell pp_write_char(cell x) {
	return pp_display(x);
}

/*
 * S9fES Extentions
 */

cell pp_bit_op(cell x) {
	char		name[] = "bit-op";
	cell		op, a, b;
	static cell	mask = 0;

	if (mask == 0) {
		mask = 1;
		while (mask <= INT_SEG_LIMIT)
			mask <<= 1;
		if (mask > INT_SEG_LIMIT)
			mask >>= 1;
		mask--;
	}
	op = integer_value(name, cadr(x));
	x = cddr(x);
	a = integer_value(name, car(x));
	for (x = cdr(x); x != NIL; x = cdr(x)) {
		b = integer_value(name, car(x));
		if (a & ~mask || b & ~mask || a < 0 || b < 0)
			return FALSE;
		switch (op) {
		case  0: a =  0;        break;
		case  1: a =   a &  b;  break;
		case  2: a =   a & ~b;  break;
		case  3: a =   a;       break;
		case  4: a =  ~a &  b;  break;
		case  5: a =        b;  break;
		case  6: a =   a ^  b;  break;
		case  7: a =   a |  b;  break;
		case  8: a = ~(a |  b); break;
		case  9: a = ~(a ^  b); break;
		case 10: a =       ~b;  break;
		case 11: a =   a | ~b;  break;
		case 12: a =  ~a;       break;
		case 13: a =  ~a |  b;  break;
		case 14: a = ~(a &  b); break;
		case 15: a = ~0;        break;
		case 16: a = a  <<  b;  break;
		case 17: a = a  >>  b;  break;
		default: return FALSE;  break;
		}
		a &= mask;
	}
	return make_integer(a);
}

char *copy_string(char *s) {
	char	*new;

	new = malloc(strlen(s)+1);
	if (s == NULL)
		fatal("copy_string(): out of memory");
	strcpy(new, s);
	return new;
}

void dump_image(char *p);

cell pp_dump_image(cell x) {
	char	*path = copy_string(string(cadr(x)));
	FILE	*f;

	f = fopen(string(cadr(x)), "r");
	if (f != NULL) {
		fclose(f);
		return error("dump-image: file exists", cadr(x));
	}
	dump_image(path);
	free(path);
	return UNSPECIFIC;
}

cell pp_delete_file(cell x) {
	if (remove(string(cadr(x))) < 0)
		error("delete-file: file does not exist", cadr(x));
	return UNSPECIFIC;
}

cell pp_error(cell x) {
	return error(string(cadr(x)), length(x) > 2? caddr(x): NOEXPR);
}

cell pp_file_exists_p(cell x) {
	FILE	*f;

	f = fopen(string(cadr(x)), "r");
	if (f == NULL)
		return FALSE;
	fclose(f);
	return TRUE;
}

cell gensym(char *prefix) {
	static long	g = 0;
	char		s[200];

	do {
		sprintf(s, "%s%ld", prefix, g);
		g++;
	} while (find_symbol(s) != NIL);
	return symbol_ref(s);
}
cell pp_gensym(cell x) {
	char	pre[101];
	int	k;

	if (cdr(x) == NIL) {
		strcpy(pre, "g");
		k = 1;
	}
	else if (string_p(cadr(x))) {
		memcpy(pre, string(cadr(x)), 100);
		k = string_len(cadr(x));
	}
	else if (symbol_p(cadr(x))) {
		memcpy(pre, symbol_name(cadr(x)), 100);
		k = symbol_len(cadr(x));
	}
	else
		return error("gensym: expected string or symbol, got",
				cadr(x));
	if (k > 100)
		return error("gensym: prefix too long", cadr(x));
	pre[100] = 0;
	return gensym(pre);
}

cell expand_syntax(cell x);

cell pp_macro_expand(cell x) {
	x = cadr(x);
	save(x);
	x = expand_syntax(x);
	unsave(1);
	return x;
}

cell expand_syntax_1(cell x);

cell pp_macro_expand_1(cell x) {
	x = cadr(x);
	save(x);
	x = expand_syntax_1(x);
	unsave(1);
	return x;
}

cell pp_reverse_b(cell x) {
	cell	n, m, h;

	m = NIL;
	n = cadr(x);
	while (n != NIL) {
		if (constant_p(n))
			return error("reverse!: immutable object", n);
		if (!pair_p(n))
			return error("reverse!: expected list, got", cadr(x));
		h = cdr(n);
		cdr(n) = m;
		m = n;
		n = h;
	}
	return m;
}

cell pp_set_input_port_b(cell x) {
	Input_port = port_no(cadr(x));
	return UNSPECIFIC;
}

cell pp_set_output_port_b(cell x) {
	Output_port = port_no(cadr(x));
	return UNSPECIFIC;
}

cell pp_stats(cell x) {
	cell	n, m;
	int	o_run_stats;

	gcv(); /* start from a known state */
	reset_counter(&Reductions);
	reset_counter(&Conses);
	reset_counter(&Nodes);
	reset_counter(&Collections);
	o_run_stats = Run_stats;
	Run_stats = 1;
	Cons_stats = 0;
	n = eval(cadr(x));
	save(n);
	Run_stats = o_run_stats;
	n = counter_to_list(&Collections);
	n = cons(n, NIL);
	save(n);
	car(Stack) = n;
	m = counter_to_list(&Nodes);
	n = cons(m, n);
	car(Stack) = n;
	m = counter_to_list(&Conses);
	n = cons(m, n);
	car(Stack) = n;
	m = counter_to_list(&Reductions);
	n = cons(m, n);
	n = cons(unsave(2), n);
	return n;
}

cell pp_symbols(cell x) {
	cell	n, a, y, new;

	n = NIL;
	a = NIL;
	for (y=Symbols; y != NIL; y = cdr(y)) {
		if (n == NIL) {
			n = a = cons(car(y), NIL);
			save(n);
		}
		else {
			new = cons(car(y), NIL);
			cdr(a) = new;
			a = cdr(a);
		}
	}
	if (n != NIL)
		unsave(1);
	return n;
}

cell pp_trace(cell x) {
	cell	n = Trace_list;

	if (cdr(x) == NIL) {
		n = Trace_list;
		Trace_list = NIL;
	}
	if (cddr(x) == NIL && cadr(x) == TRUE) {
		Trace_list = TRUE;
	}
	else {
		if (Trace_list == TRUE)
			Trace_list = NIL;
		x = cdr(x);
		while (x != NIL) {
			if (!symbol_p(car(x)))
				return error("trace: expected symbol, got",
					car(x));
			Trace_list = cons(car(x), Trace_list);
			x = cdr(x);
		}
	}
	return n;
}

#ifdef unix

cell pp_argv(cell x) {
	cell	n;
	char	**cl;

	if (Command_line == NULL || *Command_line == NULL)
		return FALSE;
	n = integer_value("argv", cadr(x));
	cl = Command_line;
	for (; n--; cl++)
		if (*cl == NULL)
			return FALSE;
	return *cl == NULL? FALSE: make_string(*cl, strlen(*cl));
}

cell pp_environ(cell x) {
	char	*s;

	s = getenv(string(cadr(x)));
	if (s == NULL)
		return FALSE;
	return make_string(s, strlen(s));
}

cell pp_system(cell x) {
	int	r;

	r = system(string(cadr(x)));
	return make_integer(r >> 8);
}
#endif

/*
 * Evaluator
 */

PRIM Primitives[] = {
 { "abs",                 pp_abs,                 1,  1, { REA,___,___ } },
 { "append2",             pp_append2,             2,  2, { LST,___,___ } },
 { "apply",               pp_apply,               2, -1, { PRC,___,___ } },
 { "assq",                pp_assq,                2,  2, { ___,LST,___ } },
 { "assv",                pp_assv,                2,  2, { ___,LST,___ } },
 { "bit-op",              pp_bit_op,              3, -1, { INT,INT,INT } },
 { "boolean?",            pp_boolean_p,           1,  1, { ___,___,___ } },
 { "caar",                pp_caar,                1,  1, { PAI,___,___ } },
 { "cadr",                pp_cadr,                1,  1, { PAI,___,___ } },
 { "cdar",                pp_cdar,                1,  1, { PAI,___,___ } },
 { "cddr",                pp_cddr,                1,  1, { PAI,___,___ } },
 { "caaar",               pp_caaar,               1,  1, { PAI,___,___ } },
 { "caadr",               pp_caadr,               1,  1, { PAI,___,___ } },
 { "cadar",               pp_cadar,               1,  1, { PAI,___,___ } },
 { "caddr",               pp_caddr,               1,  1, { PAI,___,___ } },
 { "call/cc",             pp_call_cc,             1,  1, { PRC,___,___ } },
 { "cdaar",               pp_cdaar,               1,  1, { PAI,___,___ } },
 { "cdadr",               pp_cdadr,               1,  1, { PAI,___,___ } },
 { "cddar",               pp_cddar,               1,  1, { PAI,___,___ } },
 { "cdddr",               pp_cdddr,               1,  1, { PAI,___,___ } },
 { "caaaar",              pp_caaaar,              1,  1, { PAI,___,___ } },
 { "caaadr",              pp_caaadr,              1,  1, { PAI,___,___ } },
 { "caadar",              pp_caadar,              1,  1, { PAI,___,___ } },
 { "caaddr",              pp_caaddr,              1,  1, { PAI,___,___ } },
 { "cadaar",              pp_cadaar,              1,  1, { PAI,___,___ } },
 { "cadadr",              pp_cadadr,              1,  1, { PAI,___,___ } },
 { "caddar",              pp_caddar,              1,  1, { PAI,___,___ } },
 { "cadddr",              pp_cadddr,              1,  1, { PAI,___,___ } },
 { "cdaaar",              pp_cdaaar,              1,  1, { PAI,___,___ } },
 { "cdaadr",              pp_cdaadr,              1,  1, { PAI,___,___ } },
 { "cdadar",              pp_cdadar,              1,  1, { PAI,___,___ } },
 { "cdaddr",              pp_cdaddr,              1,  1, { PAI,___,___ } },
 { "cddaar",              pp_cddaar,              1,  1, { PAI,___,___ } },
 { "cddadr",              pp_cddadr,              1,  1, { PAI,___,___ } },
 { "cdddar",              pp_cdddar,              1,  1, { PAI,___,___ } },
 { "cddddr",              pp_cddddr,              1,  1, { PAI,___,___ } },
 { "car",                 pp_car,                 1,  1, { PAI,___,___ } },
 { "cdr",                 pp_cdr,                 1,  1, { PAI,___,___ } },
 { "char?",               pp_char_p,              1,  1, { ___,___,___ } },
 { "char->integer",       pp_char_to_integer,     1,  1, { CHR,___,___ } },
 { "char-alphabetic?",    pp_char_alphabetic_p,   1,  1, { CHR,___,___ } },
 { "char-ci<=?",          pp_char_ci_le_p,        2, -1, { CHR,___,___ } },
 { "char-ci<?",           pp_char_ci_lt_p,        2, -1, { CHR,___,___ } },
 { "char-ci=?",           pp_char_ci_eq_p,        2, -1, { CHR,___,___ } },
 { "char-ci>=?",          pp_char_ci_ge_p,        2, -1, { CHR,___,___ } },
 { "char-ci>?",           pp_char_ci_gt_p,        2, -1, { CHR,___,___ } },
 { "char-downcase",       pp_char_downcase,       1,  1, { CHR,___,___ } },
 { "char-lower-case?",    pp_char_lower_case_p,   1,  1, { CHR,___,___ } },
 { "char-numeric?",       pp_char_numeric_p,      1,  1, { CHR,___,___ } },
 { "char-upcase",         pp_char_upcase,         1,  1, { CHR,___,___ } },
 { "char-upper-case?",    pp_char_upper_case_p,   1,  1, { CHR,___,___ } },
 { "char-whitespace?",    pp_char_whitespace_p,   1,  1, { CHR,___,___ } },
 { "char<=?",             pp_char_le_p,           2, -1, { CHR,___,___ } },
 { "char<?",              pp_char_lt_p,           2, -1, { CHR,___,___ } },
 { "char=?",              pp_char_eq_p,           2, -1, { CHR,___,___ } },
 { "char>=?",             pp_char_ge_p,           2, -1, { CHR,___,___ } },
 { "char>?",              pp_char_gt_p,           2, -1, { CHR,___,___ } },
 { "close-input-port",    pp_close_input_port,    1,  1, { INP,___,___ } },
 { "close-output-port",   pp_close_output_port,   1,  1, { OUP,___,___ } },
 { "cons",                pp_cons,                2,  2, { ___,___,___ } },
 { "current-input-port",  pp_current_input_port,  0,  0, { ___,___,___ } },
 { "current-output-port", pp_current_output_port, 0,  0, { ___,___,___ } },
 { "delete-file",         pp_delete_file,         1,  1, { STR,___,___ } },
 { "display",             pp_display,             1,  2, { ___,OUP,___ } },
 { "dump-image",          pp_dump_image,          1,  1, { STR,___,___ } },
 { "eof-object?",         pp_eof_object_p,        1,  1, { ___,___,___ } },
 { "eq?",                 pp_eq_p,                2,  2, { ___,___,___ } },
 { "=",                   pp_equal,               2, -1, { REA,___,___ } },
 { "eqv?",                pp_eqv_p,               2,  2, { ___,___,___ } },
 { "even?",               pp_even_p,              1,  1, { REA,___,___ } },
 { "error",               pp_error,               1,  2, { STR,___,___ } },
 { "file-exists?",        pp_file_exists_p,       1,  1, { STR,___,___ } },
 { "gensym",              pp_gensym,              0,  1, { ___,___,___ } },
 { ">",                   pp_greater,             2, -1, { REA,___,___ } },
 { ">=",                  pp_greater_equal,       2, -1, { REA,___,___ } },
 { "input-port?",         pp_input_port_p,        1,  1, { ___,___,___ } },
 { "integer?",            pp_integer_p,           1,  1, { ___,___,___ } },
 { "integer->char",       pp_integer_to_char,     1,  1, { INT,___,___ } },
 { "length",              pp_length,              1,  1, { LST,___,___ } },
 { "<",                   pp_less,                2, -1, { REA,___,___ } },
 { "<=",                  pp_less_equal,          2, -1, { REA,___,___ } },
 { "list",                pp_list,                0, -1, { ___,___,___ } },
 { "list->string",        pp_list_to_string,      1,  1, { LST,___,___ } },
 { "list->vector",        pp_list_to_vector,      1,  1, { LST,___,___ } },
 { "list-tail",           pp_list_tail,           2,  2, { LST,INT,___ } },
 { "load",                pp_load,                1,  1, { STR,___,___ } },
 { "macro-expand",        pp_macro_expand,        1,  1, { ___,___,___ } },
 { "macro-expand-1",      pp_macro_expand_1,      1,  1, { ___,___,___ } },
 { "make-string",         pp_make_string,         1,  2, { INT,CHR,___ } },
 { "make-vector",         pp_make_vector,         1,  2, { INT,___,___ } },
 { "max",                 pp_max,                 1, -1, { REA,___,___ } },
 { "memq",                pp_memq,                2,  2, { ___,LST,___ } },
 { "memv",                pp_memv,                2,  2, { ___,LST,___ } },
 { "min",                 pp_min,                 1, -1, { REA,___,___ } },
 { "-",                   pp_minus,               1, -1, { REA,___,___ } },
 { "negative?",           pp_negative_p,          1,  1, { REA,___,___ } },
 { "not",                 pp_not,                 1,  1, { ___,___,___ } },
 { "null?",               pp_null_p,              1,  1, { ___,___,___ } },
 { "odd?",                pp_odd_p,               1,  1, { REA,___,___ } },
 { "open-input-file",     pp_open_input_file,     1,  1, { STR,___,___ } },
 { "open-output-file",    pp_open_output_file,    1,  1, { STR,___,___ } },
 { "output-port?",        pp_output_port_p,       1,  1, { ___,___,___ } },
 { "pair?",               pp_pair_p,              1,  1, { ___,___,___ } },
 { "peek-char",           pp_peek_char,           0,  1, { INP,___,___ } },
 { "+",                   pp_plus,                0, -1, { REA,___,___ } },
 { "positive?",           pp_positive_p,          1,  1, { REA,___,___ } },
 { "procedure?",          pp_procedure_p,         1,  1, { ___,___,___ } },
 { "quotient",            pp_quotient,            2,  2, { REA,REA,___ } },
 { "read",                pp_read,                0,  1, { INP,___,___ } },
 { "read-char",           pp_read_char,           0,  1, { INP,___,___ } },
 { "remainder",           pp_remainder,           2,  2, { REA,REA,___ } },
 { "reverse",             pp_reverse,             1,  1, { LST,___,___ } },
 { "reverse!",            pp_reverse_b,           1,  1, { LST,___,___ } },
 { "set-car!",            pp_set_car_b,           2,  2, { PAI,___,___ } },
 { "set-cdr!",            pp_set_cdr_b,           2,  2, { PAI,___,___ } },
 { "set-input-port!",     pp_set_input_port_b,    1,  1, { INP,___,___ } },
 { "set-output-port!",    pp_set_output_port_b,   1,  1, { OUP,___,___ } },
 { "stats",               pp_stats,               1,  1, { ___,___,___ } },
 { "string",              pp_string,              0, -1, { CHR,___,___ } },
 { "string->list",        pp_string_to_list,      1,  1, { STR,___,___ } },
 { "string->symbol",      pp_string_to_symbol,    1,  1, { STR,___,___ } },
 { "string-append",       pp_string_append,       0, -1, { STR,___,___ } },
 { "string-copy",         pp_string_copy,         1,  1, { STR,___,___ } },
 { "string-fill!",        pp_string_fill_b,       2,  2, { STR,CHR,___ } },
 { "string-length",       pp_string_length,       1,  1, { STR,___,___ } },
 { "string-ref",          pp_string_ref,          2,  2, { STR,INT,___ } },
 { "string-set!",         pp_string_set_b,        3,  3, { STR,INT,CHR } },
 { "string-ci<=?",        pp_string_ci_le_p,      2, -1, { STR,___,___ } },
 { "string-ci<?",         pp_string_ci_lt_p,      2, -1, { STR,___,___ } },
 { "string-ci=?",         pp_string_ci_eq_p,      2, -1, { STR,___,___ } },
 { "string-ci>=?",        pp_string_ci_ge_p,      2, -1, { STR,___,___ } },
 { "string-ci>?",         pp_string_ci_gt_p,      2, -1, { STR,___,___ } },
 { "string<=?",           pp_string_le_p,         2, -1, { STR,___,___ } },
 { "string<?",            pp_string_lt_p,         2, -1, { STR,___,___ } },
 { "string=?",            pp_string_eq_p,         2, -1, { STR,___,___ } },
 { "string>=?",           pp_string_ge_p,         2, -1, { STR,___,___ } },
 { "string>?",            pp_string_gt_p,         2, -1, { STR,___,___ } },
 { "string?",             pp_string_p,            1,  1, { ___,___,___ } },
 { "substring",           pp_substring,           3,  3, { STR,INT,INT } },
 { "symbol?",             pp_symbol_p,            1,  1, { ___,___,___ } },
 { "symbol->string",      pp_symbol_to_string,    1,  1, { SYM,___,___ } },
 { "symbols",             pp_symbols,             0,  0, { ___,___,___ } },
 { "*",                   pp_times,               0, -1, { REA,___,___ } },
 { "trace",               pp_trace,               0, -1, { ___,___,___ } },
 { "unquote",             pp_unquote,             1,  1, { ___,___,___ } },
 { "unquote-splicing",    pp_unquote_splicing,    1,  1, { ___,___,___ } },
 { "vector",              pp_vector,              0, -1, { ___,___,___ } },
 { "vector-append",       pp_vector_append,       0, -1, { VEC,___,___ } },
 { "vector-copy",         pp_vector_copy,         1, -1, { VEC,INT,INT } },
 { "vector-fill!",        pp_vector_fill_b,       2,  2, { VEC,___,___ } },
 { "vector-length",       pp_vector_length,       1,  1, { VEC,___,___ } },
 { "vector-set!",         pp_vector_set_b,        3,  3, { VEC,INT,___ } },
 { "vector-ref",          pp_vector_ref,          2,  2, { VEC,INT,___ } },
 { "vector->list",        pp_vector_to_list,      1,  1, { VEC,___,___ } },
 { "vector?",             pp_vector_p,            1,  1, { ___,___,___ } },
 { "write",               pp_write,               1,  2, { ___,OUP,___ } },
 { "write-char",          pp_write_char,          1,  2, { CHR,OUP,___ } },
 { "zero?",               pp_zero_p,              1,  1, { REA,___,___ } },
#ifdef REALNUM
 { "/",                   pp_divide,              1, -1, { REA,___,___ } },
 { "exact->inexact",      pp_exact_to_inexact,    1,  1, { REA,___,___ } },
 { "exact?",              pp_exact_p,             1,  1, { REA,___,___ } },
 { "exponent",            pp_exponent,            1,  1, { REA,___,___ } },
 { "floor",               pp_floor,               1,  1, { REA,___,___ } },
 { "inexact->exact",      pp_inexact_to_exact,    1,  1, { REA,___,___ } },
 { "inexact?",            pp_inexact_p,           1,  1, { REA,___,___ } },
 { "mantissa",            pp_mantissa,            1,  1, { REA,___,___ } },
 { "real?",               pp_real_p,              1,  1, { ___,___,___ } },
#endif /* REALNUM */
#ifdef unix
 { "argv",                pp_argv,                1,  1, { INT,___,___ } },
 { "environ",             pp_environ,             1,  1, { STR,___,___ } },
 { "system",              pp_system,              1,  1, { STR,___,___ } },
#endif
 { NULL }
};

cell expected(cell who, char *what, cell got) {
	char	msg[100];
	PRIM	*p;

	p = (PRIM *) cadr(who);
	sprintf(msg, "%s: expected %s, got", p->name, what);
	return error(msg, got);
}

cell apply_primitive(cell x) {
	PRIM	*p;
	cell	a;
	int	k, na, i;

	p = (PRIM *) cadar(x);
	k = length(x);
	if (k-1 < p->min_args)
		return too_few_args(x);
	if (k-1 > p->max_args && p->max_args >= 0)
		return too_many_args(x);
	a = cdr(x);
	na = p->max_args < 0? p->min_args: p->max_args;
	if (na > k-1)
		na = k-1;
	for (i=1; i<=na; i++) {
		switch (p->arg_types[i-1]) {
		case T_NONE:
			break;
		case T_BOOLEAN:
			if (!boolean_p(car(a)))
				return expected(car(x), "boolean", car(a));
			break;
		case T_CHAR:
			if (!char_p(car(a)))
				return expected(car(x), "char", car(a));
			break;
		case T_INPUT_PORT:
			if (!input_port_p(car(a)))
				return expected(car(x), "input-port", car(a));
			break;
		case T_INTEGER:
			if (!integer_p(car(a)))
				return expected(car(x), "integer", car(a));
			break;
		case T_OUTPUT_PORT:
			if (!output_port_p(car(a)))
				return expected(car(x), "output-port", car(a));
			break;
		case T_PAIR:
			if (atom_p(car(a)))
				return expected(car(x), "pair", car(a));
			break;
		case T_PAIR_OR_NIL:
			if (car(a) != NIL && atom_p(car(a)))
				return expected(car(x), "list", car(a));
			break;
		case T_PROCEDURE:
			if (	!procedure_p(car(a)) &&
				!primitive_p(car(a)) &&
				!continuation_p(car(a))
			)
				return expected(car(x), "procedure", car(a));
			break;
		case T_REAL:
			if (!integer_p(car(a)) && !real_p(car(a)))
				return expected(car(x), "number", car(a));
			break;
		case T_STRING:
			if (!string_p(car(a)))
				return expected(car(x), "string", car(a));
			break;
		case T_SYMBOL:
			if (!symbol_p(car(a)))
				return expected(car(x), "symbol", car(a));
			break;
		case T_VECTOR:
			if (!vector_p(car(a)))
				return expected(car(x), "vector", car(a));
			break;
		}
		a = cdr(a);
	}
	return (*p->handler)(x);
}

int uses_transformer_p(cell x) {
	cell	y;

	if (atom_p(x) || car(x) == S_quote)
		return 0;
	if (pair_p(x) && symbol_p(car(x))) {
		y = lookup(car(x), Environment, 0);
		if (y != NIL && syntax_p(binding_value(y)))
			return 1;
	}
	while (pair_p(x)) {
		if (uses_transformer_p(car(x)))
			return 1;
		x = cdr(x);
	}
	return 0;
}

cell _eval(cell x, int cbn);

cell expand_syntax_1(cell x) {
	cell	y, m, n, a, app;

	if (Error_flag || atom_p(x) || car(x) == S_quote)
		return x;
	if (symbol_p(car(x))) {
		y = lookup(car(x), Environment, 0);
		if (y != NIL && syntax_p(binding_value(y))) {
			save(x);
			app = cons(cdr(binding_value(y)), cdr(x));
			unsave(1);
			return _eval(app, 1);
		}
	}
	/*
	 * If DEFINE-SYNTAX is followed by (MACRO-NAME ...)
	 * unbind the MACRO-NAME first to avoid erroneous
	 * expansion.
	 */
	if (	car(x) == S_define_syntax &&
		cdr(x) != NIL &&
		pair_p(cadr(x))
	) {
		m = lookup(caadr(x), Environment, 0);
		if (m != NIL)
			binding_value(m) = UNDEFINED;
	}
	n = a = NIL;
	save(n);
	while (pair_p(x)) {
		m = cons(expand_syntax_1(car(x)), NIL);
		if (n == NIL) {
			n = m;
			car(Stack) = n;
			a = n;
		}
		else {
			cdr(a) = m;
			a = cdr(a);
		}
		x = cdr(x);
	}
	cdr(a) = x;
	unsave(1);
	return n;
}

cell expand_syntax(cell x) {
	if (Error_flag || atom_p(x) || car(x) == S_quote)
		return x;
	save(x);
	while (!Error_flag) {
		if (!uses_transformer_p(x))
			break;
		x = expand_syntax_1(x);
		car(Stack) = x;
	}
	unsave(1);
	return x;
}

cell restore_state(void) {
	cell	v;

	if (State_stack == NIL)
		fatal("restore_state: stack underflow");
	v = car(State_stack);
	State_stack = cdr(State_stack);
	return v;
}

cell bind_arguments(cell n, int tail) {
	cell	p, v, a;
	cell	rib;

	save(Environment);
	p = car(n);
	a = cdr(n);
	v = cadr(p);
	Environment = cdddr(p);
	rib = NIL;
	save(rib);
	while (pair_p(v)) {
		if (atom_p(a)) {
			unsave(1);
			return too_few_args(n);
		}
		Tmp = make_binding(car(v), car(a));
		rib = cons(Tmp, rib);
		car(Stack) = rib;
		v = cdr(v);
		a = cdr(a);
	}
	if (symbol_p(v)) {
		Tmp = make_binding(v, a);
		rib = cons(Tmp, rib);
		car(Stack) = rib;
	}
	else if (a != NIL) {
		unsave(1);
		return too_many_args(n);
	}
	Tmp = NIL;
	unsave(1);
	Environment = make_env(rib, Environment);
	return UNSPECIFIC;
}

int tail_call(void) {
	if (State_stack == NIL || car(State_stack) != EV_BETA)
		return 0;
	Tmp = unsave(1);
	Environment = car(Stack);
	unsave(2);
	restore_state();
	save(Tmp);
	Tmp = NIL;
	return 1;
}

void trace(cell name, cell expr) {
	if (Error_flag)
		return;
	if (	Trace_list == TRUE ||
		memqv("trace", 0, name, Trace_list) != FALSE
	) {
		pr("+ ");
		print_form(cons(name, cdr(expr)));
		nl();
	}
}

cell _eval(cell x, int cbn) {
	cell	m2,	/* Root of result list */
		a,	/* Used to append to result */
		rib;	/* Temp storage for args */
	int	s,	/* Current state */
		c;	/* Continue */
	cell	name;	/* Name of procedure to apply */

	save(x);
	save(State_stack);
	save(Stack_bottom);
	Stack_bottom = Stack;
	s = EV_ATOM;
	c = 0;
	while (!Error_flag) {
		if (Run_stats)
			count(&Reductions);
		if (symbol_p(x)) {		/* Symbol -> Value */
			if (cbn) {
				Acc = x;
				cbn = 0;
			}
			else {
				Acc = lookup(x, Environment, 1);
				if (Error_flag)
					break;
				Acc = box_value(Acc);
			}
		}
		else if (auto_quoting_p(x) || cbn == 2) {
			Acc = x;		/* Object -> Object */
			cbn = 0;
		}
		else {				/* (...) -> Value */
			/*
			 * This block is used to DESCEND into lists.
			 * The following structure is saved on the
			 * Stack: RIB = (args append result source)
			 * The current s is saved on the State_stack.
			 */
			Acc = x;
			x = car(x);
			save_state(s);
			/* Check call-by-name built-ins and flag */
			if (special_p(x) || cbn) {
				cbn = 0;
				rib = cons(Acc, Acc);	/* result/source */
				rib = cons(NIL, rib);	/* append */
				rib = cons(NIL, rib);	/* args */
				if (!proper_list_p(Acc))
					error("syntax error", Acc);
				x = NIL;
			}
			else {
				Tmp = cons(NIL, NIL);
				rib = cons(Tmp, Acc);	/* result/source */
				rib = cons(Tmp, rib);	/* append */
				rib = cons(cdr(Acc), rib); /* args */
				Tmp = NIL;
				x = car(Acc);
			}
			save(rib);
			s = EV_ARGS;
			continue;
		}
		/*
		 * The following loop is used to ASCEND back to the
		 * root of a list, thereby performing BETA REDUCTION.
		 */
		while (!Error_flag)
		if (s == EV_BETA) {
			/* Finish BETA reduction */
			Environment = unsave(1);
			unsave(1);		/* source expression */
			s = restore_state();
		}
		else if (s == EV_ARGS) {	/* append to list, reduce */
			rib = car(Stack);
			x = rib_args(rib);
			a = rib_append(rib);
			m2 = rib_result(rib);
			if (a != NIL) 	/* Append new member */
				car(a) = Acc;
			if (x == NIL) {	/* End of list */
				Acc = m2;
				/* Remember name of caller */
				name = car(rib_source(rib));
				if (Trace_list != NIL)
					trace(name, Acc);
				if (primitive_p(car(Acc))) {
					if ((PRIM *) cadar(Acc) == Apply_magic)
						c = cbn = 1;
					if ((PRIM *) cadar(Acc) == Call_magic)
						c = cbn = 1;
					Cons_stats = 1;
					Acc = x = apply_primitive(Acc);
					Cons_stats = 0;
				}
				else if (special_p(car(Acc))) {
					Acc = x = apply_special(Acc, &c, &s);
				}
				else if (procedure_p(car(Acc))) {
					name = symbol_p(name)? name: NIL;
					Called_procedures[Proc_ptr] = name;
					Proc_ptr++;
					if (Proc_ptr >= Proc_max)
						Proc_ptr = 0;
					bind_arguments(Acc, tail_call());
					x = caddar(Acc);
					c = 2;
					s = EV_BETA;
				}
				else if (continuation_p(car(Acc))) {
					Acc = resume(Acc);
				}
				else {
					error("application of non-procedure",
						name);
					x = NIL;
				}
				if (c != 2) {
					unsave(1); /* drop source expr */
					s = restore_state();
				}
				/* Leave the ASCENDING loop and descend */
				/* once more into X. */
				if (c)
					break;
			}
			else if (atom_p(x)) {
				error("syntax error", rib_source(rib));
				x = NIL;
				break;
			}
			else {		/* X =/= NIL: append to list */
				/* Create space for next argument */
				Acc = cons(NIL, NIL);
				cdr(a) = Acc;
				rib_append(rib) = cdr(a);
				rib_args(rib) = cdr(x);
				x = car(x);	/* evaluate next member */
				break;
			}
		}
		else if (s == EV_IF_PRED) {
			x = unsave(1);
			unsave(1);	/* source expression */
			s = restore_state();
			if (Acc != FALSE)
				x = cadr(x);
			else
				x = caddr(x);
			c = 1;
			break;
		}
		else if (s == EV_AND || s == EV_OR) {
			Stack = cons(cdar(Stack), cdr(Stack));
			if (	(Acc == FALSE && s == EV_AND) ||
				(Acc != FALSE && s == EV_OR) ||
				car(Stack) == NIL
			) {
				unsave(2);	/* state, source expr */
				s = restore_state();
				x = Acc;
				cbn = 2;
			}
			else if (cdar(Stack) == NIL) {
				x = caar(Stack);
				unsave(2);	/* state, source expr */
				s = restore_state();
			}
			else {
				x = caar(Stack);
			}
			c = 1;
			break;
		}
		else if (s == EV_COND) {
			char	cond_err[] = "cond: invalid syntax";

			if (Acc != FALSE) {
				x = cdaar(Stack);
				if (x == NIL) {
					x = quote(Acc, S_quote);
				}
				else if (pair_p(cdr(x))) {
					if (car(x) == S_arrow) {
						if (cddr(x) != NIL)
							error(cond_err, x);
						Acc = quote(Acc, S_quote);
						Acc = cons(Acc, NIL);
						Acc = x = cons(cadr(x), Acc);
					}
					else {
						Acc = x = cons(S_begin, x);
					}
				}
				else {
					x = car(x);
				}
				unsave(2);	/* state, source expr */
				s = restore_state();
			}
			else if (cdar(Stack) == NIL)  {
				unsave(2);	/* state, source expr */
				s = restore_state();
				x = UNSPECIFIC;
			}
			else {
				Stack = cons(cdar(Stack), cdr(Stack));
				x = caaar(Stack);
				if (x == S_else && cdar(Stack) == NIL)
					x = TRUE;
			}
			c = 1;
			break;
		}
		else if (s == EV_BEGIN) {
			Stack = cons(cdar(Stack), cdr(Stack));
			if (cdar(Stack) == NIL) {
				x = caar(Stack);
				unsave(2);	/* state, source expr */
				s = restore_state();
			}
			else {
				x = caar(Stack);
			}
			c = 1;
			break;
		}
		else if (s == EV_SET_VAL || s == EV_MACRO) {
			char err[] = "define-syntax: expected procedure, got";

			if (s == EV_MACRO) {
				if (procedure_p(Acc)) {
					Acc = new_atom(T_SYNTAX, Acc);
				}
				if (syntax_p(Acc)) {
					/* Acc = Acc; */
				}
				else {
					error(err, Acc);
					break;
				}
			}
			x = unsave(1);
			unsave(1);	/* source expression */
			s = restore_state();
			box_value(x) = Acc;
			Acc = x = UNSPECIFIC;
			c = 0;
			break;
		}
		else { /* s == EV_ATOM */
			break;
		}
		if (c) {	/* Continue evaluation if requested */
			c = 0;
			continue;
		}
		if (Stack == Stack_bottom)
			break;
	}
	Stack = Stack_bottom;
	Stack_bottom = unsave(1);
	State_stack = unsave(1);
	unsave(1);
	return Acc;		/* Return the evaluated expr */
}

void reset_calltrace(void) {
	int	i;

	for (i=0; i<MAX_CALL_TRACE; i++)
		Called_procedures[i] = NIL;
}

cell eval(cell x) {
	reset_calltrace();
	save(x);
	x = expand_syntax(x);
	unsave(1);
	x = _eval(x, 0);
	return x;
}

/*
 * REPL
 */

void clear_leftover_envs(void) {
	while (cdr(Environment) != NIL)
		Environment = cdr(Environment);
}

#ifndef NO_SIGNALS
 void keyboard_interrupt(int sig) {
	Input_port = 0;
	Output_port = 1;
	error("interrupted", NOEXPR);
	signal(SIGINT, keyboard_interrupt);
 }

 void keyboard_quit(int sig) {
	reset_tty();
	fatal("received quit signal, exiting");
 }

 void terminated(int sig) {
	bye(1);
 }
#endif

void repl(void) {
	cell	n = NIL; /*LINT*/
	cell	sane_env;

	sane_env = cons(NIL, NIL);
	save(sane_env);
	if (!Quiet_mode) {
		signal(SIGINT, keyboard_interrupt);
	}
	while (1) {
		reset_tty();
		Error_flag = 0;
		Input_port = 0;
		Output_port = 1;
		clear_leftover_envs();
		reset_calltrace();
		car(sane_env) = Environment;
		if (!Quiet_mode) {
			pr("> "); flush();
		}
		Program = xread();
		if (Program == END_OF_FILE)
			break;
		if (!Error_flag)
			n = eval(Program);
		if (!Error_flag && !unspecific_p(n)) {
			print_form(n);
			pr("\n");
			box_value(S_latest) = n;
		}
		if (Error_flag)
			Environment = car(sane_env);
	}
	unsave(1);
	pr("\n");
}

/*
 * Startup and Initialization
 */

/* Variables to dump to image file */
cell *Image_vars[] = {
	&Free_list, &Free_vecs, &Symbols, &Environment,
	&S_arrow, &S_else, &S_extensions, &S_latest,
	&S_library_path, &S_loading, &S_quasiquote,
	&S_quote, &S_unquote, &S_unquote_splicing,
	&S_and, &S_begin, &S_cond, &S_define, &S_define_syntax,
	&S_if, &S_lambda, &S_or, &S_quote, &S_set_b,
NULL };

struct magic {
	char	id[2];			/* "S9"		*/
	char	version[10];		/* "yyyy-mm-dd"	*/
	char	cell_size[1];		/* size + '0'	*/
	char    mantissa_size[1];	/* size + '0'	*/
	char	_pad[2];		/* "__"		*/
	char	byte_order[8];		/* e.g. "4321"	*/
	char	binary_id[8];		/* see code	*/
};

void dump_image(char *p) {
	FILE		*f;
	cell		n, **v;
	int		i, k;
	struct magic	m;
	char		buf[100];

	f = fopen(p, "wb");
	if (f == NULL) {
		error("cannot create image file",
			make_string(p, (int) strlen(p)));
		return;
	}
	memset(&m, '_', sizeof(m));
	strncpy(m.id, "S9", sizeof(m.id));
	strncpy(m.version, VERSION, sizeof(m.version));
	m.cell_size[0] = sizeof(cell)+'0';
	m.mantissa_size[0] = MANTISSA_SEGMENTS+'0';
	n = 0x31323334L;
	memcpy(m.byte_order, &n, sizeof(n)>8? 8: sizeof(n));
	n = (cell) &Primitives;
	memcpy(m.binary_id, &n, sizeof(n)>8? 8: sizeof(n));
	fwrite(&m, sizeof(m), 1, f);
	i = Cons_pool_size;
	fwrite(&i, sizeof(int), 1, f);
	i = Vec_pool_size;
	fwrite(&i, sizeof(int), 1, f);
	v = Image_vars;
	i = 0;
	while (v[i]) {
		fwrite(v[i], sizeof(cell), 1, f);
		i++;
	}
	if (	fwrite(Car, 1, Cons_pool_size*sizeof(cell), f)
		 != Cons_pool_size*sizeof(cell) ||
		fwrite(Cdr, 1, Cons_pool_size*sizeof(cell), f)
		 != Cons_pool_size*sizeof(cell) ||
		fwrite(Tag, 1, Cons_pool_size, f) != Cons_pool_size ||
		fwrite(Vectors, 1, Vec_pool_size*sizeof(cell), f)
		 != Vec_pool_size*sizeof(cell)
	) {
		fclose(f);
		error("image dump failed", NOEXPR);
		return;
	}
	fclose(f);
	k = gc();
	if (!Quiet_mode) {
		sprintf(buf, "image dumped: %d nodes used, %d free",
				Cons_pool_size-k, k);
		pr(buf); nl();
	}
}

int load_image(char *p) {
	FILE		*f;
	cell		n, **v;
	int		i;
	struct magic	m;
	int		ok = 1;
	int		image_nodes, image_vcells;
	cell		name;

	name = make_string(p, (int) strlen(p));
	f = fopen(p, "rb");
	if (f == NULL)
		return -1;
	fread(&m, sizeof(m), 1, f);
	if (memcmp(m.id, "S9", 2)) {
		error("error in image file (magic match failed)", name);
		ok = 0;
	}
	if (memcmp(m.version, VERSION, 10)) {
		error("error in image file (wrong version)", name);
		ok = 0;
	}
	if (m.cell_size[0]-'0' != sizeof(cell)) {
		error("error in image file (wrong cell size)", name);
		ok = 0;
	}
	if (m.mantissa_size[0]-'0' != MANTISSA_SEGMENTS) {
		error("error in image file (wrong mantissa size)", name);
		ok = 0;
	}
	memcpy(&n, m.byte_order, sizeof(cell));
	if (n != 0x31323334L) {
		error("error in image file (wrong architecture)", name);
		ok = 0;
	}
	memcpy(&n, m.binary_id, sizeof(cell));
	if (n != (cell) &Primitives) {
		error("error in image file (wrong interpreter)", name);
		ok = 0;
	}
	memset(Tag, 0, Cons_pool_size);
	fread(&image_nodes, sizeof(int), 1, f);
	fread(&image_vcells, sizeof(int), 1, f);
	while (image_nodes > Cons_pool_size) {
		if (	Memory_limit_kn &&
			Cons_pool_size + Cons_segment_size > Memory_limit_kn
		) {
			error("image too big (too many conses)", NOEXPR);
			ok = 0;
			break;
		}
		new_cons_segment();
	}
	while (image_vcells > Vec_pool_size) {
		if (	Memory_limit_kn &&
			Vec_pool_size + Vec_segment_size > Memory_limit_kn
		) {
			error("image too big (too many vcells)", NOEXPR);
			ok = 0;
			break;
		}
		new_vec_segment();
	}
	v = Image_vars;
	i = 0;
	while (v[i]) {
		fread(v[i], sizeof(cell), 1, f);
		i++;
	}
	if (	ok &&
		(fread(Car, 1, image_nodes*sizeof(cell), f)
		  != image_nodes*sizeof(cell) ||
		 fread(Cdr, 1, image_nodes*sizeof(cell), f)
		  != image_nodes*sizeof(cell) ||
		 fread(Tag, 1, image_nodes, f) != image_nodes ||
		 fread(Vectors, 1, image_vcells*sizeof(cell), f)
		  != image_vcells*sizeof(cell) ||
		 fgetc(f) != EOF)
	) {
		error("error in image file (wrong size)", NOEXPR);
		ok = 1;
	}
	fclose(f);
	if (Error_flag)
		fatal("unusable image");
	return 0;
}

cell get_library_path(void) {
	char	*s;

	s = getenv("S9FES_LIBRARY_PATH");
	if (s == NULL)
		s = DEFAULT_LIBRARY_PATH;
	return make_string(s, (int) strlen(s));
}

char *libname(char *argv0) {
	char	*name;

	if (argv0 == NULL || argv0[0] == 0)
		argv0 = "s9";
	name = strrchr(argv0, '/');
	name = name? &name[1]: argv0;
	return name;
}

void load_library(char *argv0) {
	char	*path, buf[256], *p;
	char	libdir[240], libfile[256];
	char	*home, *image;
	cell	new;

	image = libname(argv0);
	path = copy_string(string(box_value(S_library_path)));
	home = getenv("HOME");
	if (home == NULL)
		home = ".";
	p = strtok(path, ":");
	while (p != NULL) {
		if (p[0] == '~') {
			if (strlen(p) + strlen(home) >= sizeof(libdir)-1)
				fatal("load_library: path too long");
			sprintf(libdir, "%s%s", home, &p[1]);
		}
		else {
			if (strlen(p) >= sizeof(libdir)-1)
				fatal("load_library: path too long");
			strcpy(libdir, p);
		}
		if (strlen(image) + strlen(libdir) + strlen(".image")
			>= sizeof(libfile)-1
		)
			fatal("load_library: path too long");
		sprintf(libfile, "%s/%s.image", libdir, image);
		if (strcmp(image, "-") && load_image(libfile) == 0) {
			free(path);
			/* *library-path* is overwritten by load_image() */
			new = get_library_path();
			box_value(S_library_path) = new;
			return;
		}
		if (strlen(image) + strlen(libdir) + strlen(".scm")
			>= sizeof(libfile)-1
		)
			fatal("load_library: path too long");
		sprintf(libfile, "%s/%s.scm", libdir,
			!strcmp(image, "-")? "s9": image);
		if (load(libfile) == 0) {
			free(path);
			return;
		}
		p = strtok(NULL, ":");
	}
	sprintf(buf, "found neither \"%s.image\" nor \"%s.scm\"", image, image);
	fatal(buf);
}

void load_rc(void) {
	char	rcpath[256];
	char	rcfile[] = "/.s9fes/rc";
	char	*home;

	home = getenv("HOME");
	if (home == NULL)
		return;
	if (strlen(home) + strlen(rcfile) + 1 >= sizeof(rcpath)-1)
		fatal("path too long in HOME");
	sprintf(rcpath, "%s/%s", home, rcfile);
	load(rcpath);
}

void add_primitives(char *name, PRIM *p) {
	cell	v, n, new;
	int	i;

	if (name) {
		n = symbol_ref(name);
		new = cons(n, box_value(S_extensions));
		box_value(S_extensions) = new;
	}
	for (i=0; p && p && p[i].name; i++) {
		if (Apply_magic == NULL && !strcmp(p[i].name, "apply"))
			Apply_magic = &p[i];
		if (Call_magic == NULL && !strcmp(p[i].name, "call/cc"))
			Call_magic = &p[i];
		v = symbol_ref(p[i].name);
		n = new_atom((cell) &p[i], NIL);
		n = new_atom(T_PRIMITIVE, n);
		Environment = extend(v, n, Environment);
	}
}

/* Extension prototypes; add your own here. */
void curs_init(void);
void sys_init(void);

void make_initial_env(void) {
	cell	new;

	Environment = cons(NIL, NIL);
	Environment = extend(symbol_ref("**"), NIL, Environment);
	S_latest = cadr(Environment);
	Environment = extend(symbol_ref("*extensions*"), NIL, Environment);
	S_extensions = cadr(Environment);
	Environment = extend(symbol_ref("*library-path*"), NIL, Environment);
	S_library_path = cadr(Environment);
	new = get_library_path();
	box_value(S_library_path) = new;
	Environment = extend(symbol_ref("*loading*"), FALSE, Environment);
	S_loading = cadr(Environment);
	Apply_magic = NULL;
	Call_magic = NULL;
	add_primitives(NULL, Primitives);
	EXTENSIONS;
#ifdef REALNUM
	add_primitives("realnums", NULL);
#endif
	Environment = cons(Environment, NIL);
	Program = TRUE; /* or rehash() will not work */
	rehash(car(Environment));
}

void init(void) {
	int	i;

	for (i=2; i<MAX_PORTS; i++)
		Ports[i] = NULL;
	Ports[0] = stdin;
	Ports[1] = stdout;
	Ports[2] = stderr;
	Port_flags[0] = LOCK_TAG;
	Port_flags[1] = LOCK_TAG;
	Port_flags[2] = LOCK_TAG;
	Input_port = 0;
	Output_port = 1;
	Error_port = 2;
	Cons_segment_size = INITIAL_SEGMENT_SIZE;
	Vec_segment_size = INITIAL_SEGMENT_SIZE;
	Cons_pool_size = 0,
	Vec_pool_size = 0;
	Car = NULL,
	Cdr = NULL;
	Tag = NULL;
	Free_list = NIL;
	Vectors = NULL;
	Free_vecs = 0;
	Memory_limit_kn = DEFAULT_LIMIT_KN * 1024L;
	Stack = NIL,
	Stack_bottom = NIL;
	State_stack = NIL;
	Tmp_car = NIL;
	Tmp_cdr = NIL;
	Tmp = NIL;
	Symbols = NIL;
	Program = NIL;
	Proc_ptr = 0;
	Proc_max = 3;
	Environment = NIL;
	Acc = NIL;
	Trace_list = NIL;
	Level = 0;
	Line_no = 1;
	Error_flag = 0;
	Load_level = 0;
	Displaying = 0;
	Printer_limit = 0;
	Quiet_mode = 0;
	Command_line = NULL;
	Run_stats = 0;
	new_cons_segment();
	new_vec_segment();
	gc();
	S_arrow = symbol_ref("=>");
	S_and = symbol_ref("and");
	S_begin = symbol_ref("begin");
	S_cond = symbol_ref("cond");
	S_define = symbol_ref("define");
	S_define_syntax = symbol_ref("define-syntax");
	S_else = symbol_ref("else");
	S_if = symbol_ref("if");
	S_lambda = symbol_ref("lambda");
	S_or = symbol_ref("or");
	S_quasiquote = symbol_ref("quasiquote");
	S_quote = symbol_ref("quote");
	S_set_b = symbol_ref("set!");
	S_unquote = symbol_ref("unquote");
	S_unquote_splicing = symbol_ref("unquote-splicing");
	make_initial_env();
	reset_calltrace();
}

void init_extensions(void) {
	cell	e, n;
	char	initproc[TOKEN_LENGTH+2];
	char	*s;
	char	*s9 = "s9";

	e = box_value(S_extensions);
	while (s9 || e != NIL) {
		s = s9? s9: string(car(e));
		if (strlen(s)*2+1 >= TOKEN_LENGTH)
			fatal("init_extension(): procedure name too long");
		sprintf(initproc, "%s:%s", s, s);
		n = find_symbol(initproc);
		if (n != NIL) {
			n = cons(n, NIL);
			eval(n);
		}
		e = s9? e: cdr(e);
		s9 = NULL;
	}
}

void usage(int quit) {
	pr("Usage: s9 [-h?] [-i name] [-gnqv] [-f prog [args]] [-m size[m]]");
	nl();
	pr("          [-l prog] [-t count] [-d image] [-- [args]]");
	nl();
	if (quit) bye(1);
}

void long_usage() {
	nl();
	usage(0);
	nl();
	pr("-h              display this summary (also -?)"); nl();
	pr("-i name         base name of image file (must be first option!)");
	nl();
	pr("-i -            ignore image, load s9.scm instead"); nl();
	pr("-d file         dump heap image to file and exit"); nl();
	pr("-f file [args]  run program and exit (implies -q)"); nl();
	pr("-g              print GC summaries (-gg = more)"); nl();
	pr("-l file         load program (may be repeated)"); nl();
	pr("-n              do not load $HOME/.s9fes/rc"); nl();
	pr("-m n[m]         set memory limit to nK (or nM) nodes"); nl();
	pr("-q              be quiet (no banner, no prompt, exit on errors)");
	nl();
	pr("-t n            list up to N procedures in call traces"); nl();
	pr("-v              print version and exit"); nl();
	pr("-- [args]       pass subsequent arguments to program"); nl();
	nl();
}

void version_info(char *name) {
	char	buf[100];
	cell	x;

	nl();
	pr("Scheme 9 from Empty Space by Nils M Holm");
	nl();
	nl();
	pr("version:         "); pr(VERSION);
#ifdef unix
	pr(" (unix)");
#else
 #ifdef plan9
	pr(" (plan 9)");
 #else
	pr(" (unknown)");
 #endif
#endif
	nl();
	pr("heap image:      ");
	if (!strcmp(name, "-"))
		pr("n/a");
	else {
		pr(name); pr(".image");
	}
	nl();
	pr("library path:    "); pr(string(box_value(S_library_path))); nl();
	pr("memory limit:    ");
	if (Memory_limit_kn) {
		sprintf(buf, "%ld", Memory_limit_kn / 1024);
		pr(buf); pr("K nodes"); nl();
	}
	else {
		pr("none"); nl();
	}
	pr("extensions:      ");
	if (box_value(S_extensions) == NIL)
		pr("-");
	for (x = box_value(S_extensions); x != NIL; x = cdr(x)) {
		print_form(car(x));
		if (cdr(x) != NIL)
			pr(" ");
	}
	nl();
#ifdef REALNUM
	pr("mantissa size:   ");
	sprintf(buf, "%d", MANTISSA_SIZE);
	pr(buf); pr(" digits"); nl();
#endif
	nl();
	pr("This program is in the public domain.");
	nl();
	nl();
}

long get_size_k(char *s) {
	int	c;
	long	n;

	c = s[strlen(s)-1];
	n = atol(s);
	if (isdigit(c))
		;
	else if (c == 'M' || c == 'm')
		n *= 1024L;
	else
		usage(1);
	return n * 1024;
}

int main(int argc, char **argv) {
	int	ignore_rc = 0;
	int	run_script;
	char	*argv0;

	if (argc > 1 && !strcmp(argv[1], "-i")) {
		if (argc < 2) {
			usage(1);
			bye(1);
		}
		argv += 2;
	}
	init();
	signal(SIGQUIT, keyboard_quit);
	signal(SIGTERM, terminated);
	load_library(argv[0]);
	argv0 = *argv++;
	init_extensions();
	while (*argv != NULL) {
		if (**argv != '-')
			break;
		(*argv)++;
		while (**argv) {
			switch (**argv)  {
			case '-':
				Command_line = ++argv;
				break;
			case 'd':
				if (argv[1] == NULL)
					usage(1);
				dump_image(argv[1]);
				bye(Error_flag? 1: 0);
				break;
			case 'f':
			case 'l':
				if (argv[1] == NULL)
					usage(1);
				run_script = **argv == 'f';
				if (run_script) {
					Quiet_mode = 1;
					Command_line = &argv[2];
				}
				if (load(argv[1]))
					error("program file not found",
						make_string(argv[1],
							(int)strlen(argv[1])));
				if (Error_flag)
					bye(1);
				if (run_script)
					bye(0);
				argv++;
				*argv = &(*argv)[strlen(*argv)];
				break;
			case 'g':
				Verbose_GC++;
				(*argv)++;
				break;
			case 'n':
				ignore_rc = 1;
				(*argv)++;
				break;
			case 'm':
				if (argv[1] == NULL)
					usage(1);
				Memory_limit_kn = get_size_k(argv[1]);
				argv++;
				*argv += strlen(*argv);
				break;
			case 'q':
				Quiet_mode = 1;
				(*argv)++;
				break;
			case 't':
				if (argv[1] == NULL)
					usage(1);
				Proc_max = atoi(argv[1]);
				if (Proc_max > MAX_CALL_TRACE)
					Proc_max = MAX_CALL_TRACE;
				argv++;
				*argv += strlen(*argv);
				break;
			case 'v':
				version_info(argv0);
				bye(0);
				break;
			case 'h':
			case '?':
				long_usage();
				bye(0);
				break;
			default:
				usage(1);
				break;
			}
			if (Command_line)
				break;
		}
		if (Command_line)
			break;
		argv++;
	}
	if (!Command_line && argv[0] != NULL)
		usage(1);
	if (!Quiet_mode)
		pr("Scheme 9 from Empty Space\n");
	if (!ignore_rc)
		load_rc();
	repl();
	reset_tty();
	return 0;
}

