#define RX_NODE_MAX 8192
#define RX_SET_MAX 64
#define RX_HINT_MAX 40
#define RX_LITERAL_MAX 256
#define RX_GROUP_MAX 9
#define RX_SLOT_MAX 20
#define RX_UNBOUNDED (-1)
#define RX_PARSE_MAX 8193

enum { RX_BYTE = 1, RX_ANY, RX_SET, RX_BEGIN, RX_END, RX_EDGE,
       RX_CAPTURE, RX_BACKREF, RX_ALT, RX_COUNT };
enum { RX_HAS_BACKREF = 2, RX_BRANCHING = 4,
       RX_FIRST_KNOWN = 8, RX_LAST_KNOWN = 16, RX_ANCHORED = 32,
       RX_LITERAL_PROVES = 64, RX_IGNORE_CASE = 128 };
enum { RX_NO_MATCH, RX_MATCH, RX_COMPLEX };
enum { REGEX_DOT_NEWLINE = 1, REGEX_LINE_ANCHORS = 2, REGEX_BASIC_REPEATS = 4,
       REGEX_POLICY_DEFAULT = 5, REGEX_POLICY_TAC = 2 };
enum { REGEX_BOUNDARY_NONE, REGEX_BOUNDARY_WORD, REGEX_BOUNDARY_LINE };
enum { REGEX_EDGE_WORD, REGEX_EDGE_NOT_WORD, REGEX_EDGE_START, REGEX_EDGE_STOP };

/* Zero ends a sequence. ALT owns two branch roots; CAPTURE and COUNT own
   a child sequence's first/last nodes. No node is copied for repetition. */
typedef struct
{
        p8 kind, argument;
        p16 next, previous, left, right;
        b16 minimum, maximum;
} rx_node;

typedef struct
{
        p8 first_skip[256], last_bytes[256], literal[RX_LITERAL_MAX];
        positive literal_length;
        positive2 literal_anchors;
} rx_hints;

typedef struct
{
        const rx_node *nodes;
        const p8 (*sets)[256];
        const rx_hints *hints;
        p16 first;
        p8 groups, policy, boundary, flags;
} regex_program;

typedef struct
{
        p16 nodes, sets, hints;
} rx_mark;

typedef struct
{
        rx_node nodes[RX_NODE_MAX];
        p8 sets[RX_SET_MAX][256];
        rx_hints hints[RX_HINT_MAX];
        rx_mark used;
} rx_pool;

typedef struct
{
        p16 first, last;
} rx_fragment;

typedef struct
{
        rx_pool *pool;
        rx_mark cursor;
        regex_program program;
        string_address pattern;
        positive length, at;
        b32 depth;
        bool extended, escapes, broken;
} rx_compiler;

static p8 rx_peek(rx_compiler *c, positive ahead)
{
        return ahead < c->length - c->at ? c->pattern[c->at + ahead] : 0;
}

static bool rx_operator(rx_compiler *c, p8 character)
{
        return c->extended ? rx_peek(c, 0) == character
                           : rx_peek(c, 0) == '\\' && rx_peek(c, 1) == character;
}

static p16 rx_emit(rx_compiler *c, rx_node node)
{
        if (c->broken || c->cursor.nodes == RX_NODE_MAX)
        {
                c->broken = true;
                return 0;
        }
        p16 at = c->cursor.nodes++;
        c->pool->nodes[at] = node;
        return at;
}

static rx_fragment rx_join(rx_compiler *c, rx_fragment one, rx_fragment two)
{
        if (!one.first)
                return two;
        if (!two.first)
                return one;
        c->pool->nodes[one.last].next = two.first;
        c->pool->nodes[two.first].previous = one.last;
        one.last = two.last;
        return one;
}

static b32 rx_new_set(rx_compiler *c)
{
        if (c->cursor.sets == RX_SET_MAX)
        {
                c->broken = true;
                return -1;
        }
        b32 set = c->cursor.sets++;
        memory_fill(c->pool->sets[set], 0, 256);
        return set;
}

static fn rx_set_add(rx_compiler *c, b32 set, p8 byte)
{
        c->pool->sets[set][byte] = 1;
        if ((c->program.flags & RX_IGNORE_CASE) && byte_is_alpha(byte))
        {
                byte ^= 32;
                c->pool->sets[set][byte] = 1;
        }
}

/* Brackets keep the BRE/ERE backslash rule; sed enables its own escapes. */
static b32 rx_parse_set(rx_compiler *c)
{
        b32 set = rx_new_set(c);
        bool negate = rx_peek(c, 0) == '^', first = true;
        if (set < 0)
                return 0;
        c->at += negate;
        while (c->at < c->length)
        {
                p8 byte = rx_peek(c, 0);
                if (byte == ']' && !first)
                {
                        c->at++;
                        if (negate)
                                for (b32 i = 0; i < 256; i++)
                                        c->pool->sets[set][i] ^= 1;
                        return set;
                }
                first = false;
                if (byte == '[' && rx_peek(c, 1) == ':')
                {
                        positive used;
                        b32 kind = byte_class_parse(c->pattern + c->at,
                                                   c->length - c->at, address_of used);
                        if (kind >= 0)
                        {
                                for (b32 i = 0; i < 256; i++)
                                        if (byte_class_holds(kind, (p8)i))
                                                rx_set_add(c, set, (p8)i);
                                c->at += used;
                                continue;
                        }
                }
                if (c->escapes && byte == '\\' && rx_peek(c, 1))
                {
                        byte = rx_peek(c, 1);
                        byte = byte == 'n' ? '\n' : byte == 't' ? '\t' :
                               byte == 'r' ? '\r' : byte;
                        c->at++;
                }
                c->at++;
                if (rx_peek(c, 0) == '-' && rx_peek(c, 1) && rx_peek(c, 1) != ']')
                {
                        p8 last = rx_peek(c, 1);
                        c->at += 2;
                        for (b32 i = byte; i <= last; i++)
                                rx_set_add(c, set, (p8)i);
                }
                else
                        rx_set_add(c, set, byte);
        }
        c->broken = true;
        return set;
}

static rx_fragment rx_alternation(rx_compiler *c);

static rx_fragment rx_atom(rx_compiler *c)
{
        p8 byte = rx_peek(c, 0), kind = RX_BYTE;
        rx_fragment child = {0};
        if (rx_operator(c, '('))
        {
                byte = c->program.groups < RX_GROUP_MAX ? ++c->program.groups : 0;
                c->at += c->extended ? 1 : 2;
                child = rx_alternation(c);
                if (!rx_operator(c, ')'))
                        c->broken = true;
                else
                        c->at += c->extended ? 1 : 2;
                if (!byte)
                        return child;
                kind = RX_CAPTURE;
        }
        else
        {
                positive at = c->at++;
                if (byte == '.')
                        kind = RX_ANY;
                else if (byte == '[')
                {
                        kind = RX_SET;
                        byte = (p8)rx_parse_set(c);
                }
                else if (byte == '^' && (c->extended || !at ||
                         (at >= 2 && c->pattern[at - 2] == '\\' &&
                          (c->pattern[at - 1] == '(' || c->pattern[at - 1] == '|'))))
                        kind = RX_BEGIN;
                else if (byte == '$' && (c->extended || c->at == c->length ||
                         rx_operator(c, ')') || rx_operator(c, '|')))
                        kind = RX_END;
                else if (byte == '\\' && rx_peek(c, 0))
                {
                        byte = rx_peek(c, 0);
                        c->at++;
                        if (byte >= '1' && byte <= '9')
                        {
                                kind = RX_BACKREF;
                                byte -= '0';
                        }
                        else if (byte == 'w' || byte == 'W' || byte == 's' || byte == 'S')
                        {
                                bool space = byte == 's' || byte == 'S';
                                bool negate = byte == 'W' || byte == 'S';
                                b32 set = rx_new_set(c);
                                if (set >= 0)
                                        for (b32 i = 0; i < 256; i++)
                                                if ((space ? byte_is_space((p8)i) : text_word((p8)i)) != negate)
                                                        rx_set_add(c, set, (p8)i);
                                kind = RX_SET;
                                byte = (p8)(set < 0 ? 0 : set);
                        }
                        else if (byte == 'b' || byte == 'B' || byte == '<' || byte == '>')
                        {
                                kind = RX_EDGE;
                                byte = byte == 'b' ? REGEX_EDGE_WORD : byte == 'B' ?
                                       REGEX_EDGE_NOT_WORD : byte == '<' ? REGEX_EDGE_START : REGEX_EDGE_STOP;
                        }
                        else if (c->escapes)
                                byte = byte == 'n' ? '\n' : byte == 't' ? '\t' : byte;
                }
                if (kind == RX_BYTE && (c->program.flags & RX_IGNORE_CASE))
                        byte = (p8)byte_to_lower(byte);
        }
        p16 at = rx_emit(c, (rx_node){.kind = kind, .argument = byte,
                                     .left = child.first, .right = child.last});
        return (rx_fragment){at, at};
}

static bool rx_interval(rx_compiler *c, b32 *low, b32 *high)
{
        positive at = c->at + (c->extended ? 1 : 2), used;
        if (at >= c->length)
                return false;
        positive first = string_digits_max(c->pattern + at, c->length - at, address_of used);
        if (!used && c->pattern[at] != ',')
                return false;
        at += used;
        positive second = first;
        if (at < c->length && c->pattern[at] == ',')
        {
                at++;
                second = string_digits_max(c->pattern + at, c->length - at, address_of used);
                at += used;
                if (!used)
                        second = positive_max;
        }
        if (first > 32767 || (second != positive_max && second > 32767))
                return false;
        if (c->extended ? at >= c->length || c->pattern[at] != '}' :
            at + 1 >= c->length || c->pattern[at] != '\\' || c->pattern[at + 1] != '}')
                return false;
        *low = (b32)first;
        *high = second == positive_max ? RX_UNBOUNDED : (b32)second;
        c->at = at + (c->extended ? 1 : 2);
        return true;
}

static rx_fragment rx_piece(rx_compiler *c)
{
        p16 body_start = c->cursor.nodes;
        rx_fragment prefix = {0}, body = rx_atom(c);
        while (!c->broken)
        {
                p8 byte = rx_peek(c, 0);
                b32 low = 0, high = RX_UNBOUNDED;
                if (byte == '*' && !c->extended && c->pool->nodes[body.first].kind == RX_BEGIN)
                {
                        c->at++;
                        prefix = rx_join(c, prefix, body);
                        body_start = c->cursor.nodes;
                        p16 at = rx_emit(c, (rx_node){.kind = RX_BYTE, .argument = '*'});
                        body = (rx_fragment){at, at};
                        continue;
                }
                if (byte == '*')
                        c->at++;
                else if ((c->extended || (c->program.policy & REGEX_BASIC_REPEATS)) &&
                         (rx_operator(c, '+') || rx_operator(c, '?')))
                {
                        low = rx_operator(c, '+');
                        high = low ? RX_UNBOUNDED : 1;
                        c->at += c->extended ? 1 : 2;
                }
                else if ((c->extended || (c->program.policy & REGEX_BASIC_REPEATS)) && rx_operator(c, '{'))
                {
                        if (!rx_interval(c, address_of low, address_of high))
                                break;
                }
                else
                        break;
                bool simple = body.first == body.last && body.first &&
                              c->pool->nodes[body.first].kind >= RX_BYTE &&
                              c->pool->nodes[body.first].kind <= RX_SET;
                if (!simple)
                {
                        if (!low && !high)
                        {
                                c->cursor.nodes = body_start;
                                body = (rx_fragment){0};
                        }
                        if (high >= 0 && high < low)
                                high = low;
                        if (high == low && (!body.first || low == 1))
                                continue;
                }
                p16 at = rx_emit(c, (rx_node){.kind = RX_COUNT,
                                             .left = body.first, .right = body.last,
                                             .minimum = (b16)low, .maximum = (b16)high});
                body = (rx_fragment){at, at};
        }
        return rx_join(c, prefix, body);
}

static rx_fragment rx_alternation(rx_compiler *c)
{
        rx_fragment whole = {0};
        bool have_alternative = false;
        if (++c->depth > RX_PARSE_MAX)
                c->broken = true;
        for (;;)
        {
                rx_fragment branch = {0};
                while (!c->broken && c->at < c->length && !rx_operator(c, ')') && !rx_operator(c, '|'))
                        branch = rx_join(c, branch, rx_piece(c));
                if (!have_alternative)
                        whole = branch;
                else
                {
                        p16 at = rx_emit(c, (rx_node){.kind = RX_ALT,
                                                     .left = whole.first, .right = branch.first});
                        whole = (rx_fragment){at, at};
                }
                if (c->broken || !rx_operator(c, '|'))
                        break;
                have_alternative = true;
                c->at += c->extended ? 1 : 2;
        }
        c->depth--;
        return whole;
}

static p16 rx_tail(rx_compiler *c, p16 at)
{
        while (at && c->pool->nodes[at].next)
                at = c->pool->nodes[at].next;
        return at;
}

/* Return 0 after a consuming edge, 1 for an empty path, 2 for an unknown
   first byte. The reverse walk passes assertions but never backreferences. */
static b32 rx_edges(rx_compiler *c, p16 first, p16 last, p8 *table, bool reverse)
{
        b32 result = 1;
        if (++c->depth > RX_PARSE_MAX)
                c->broken = true;
        for (p16 at = reverse ? last : first; at && !c->broken;)
        {
                rx_node *node = c->pool->nodes + at;
                result = 1;
                if (node->kind >= RX_BYTE && node->kind <= RX_SET)
                {
                        if (node->kind == RX_BYTE)
                        {
                                table[node->argument] = 1;
                                if ((c->program.flags & RX_IGNORE_CASE) && byte_is_alpha(node->argument))
                                        table[node->argument ^ 32] = 1;
                        }
                        else if (node->kind == RX_ANY)
                        {
                                p8 newline = table['\n'];
                                memory_fill(table, 1, 256);
                                if (!(c->program.policy & REGEX_DOT_NEWLINE))
                                        table['\n'] = newline;
                        }
                        else
                                for (b32 i = 0; i < 256; i++)
                                        table[i] |= c->pool->sets[node->argument][i];
                        result = 0;
                }
                else if (node->kind == RX_BACKREF ||
                         (!reverse && node->kind >= RX_BEGIN && node->kind <= RX_EDGE))
                        result = 2;
                else if (node->kind == RX_ALT)
                {
                        b32 one = rx_edges(c, node->left, rx_tail(c, node->left), table, reverse);
                        b32 two = rx_edges(c, node->right, rx_tail(c, node->right), table, reverse);
                        result = one == 2 || two == 2 ? 2 : one || two;
                }
                else if (node->kind == RX_CAPTURE || (node->kind == RX_COUNT && node->maximum))
                {
                        result = rx_edges(c, node->left, node->right, table, reverse);
                        if (node->kind == RX_COUNT && !node->minimum && result != 2)
                                result = 1;
                }
                if (result != 1)
                        break;
                at = reverse ? node->previous : node->next;
        }
        c->depth--;
        return c->broken ? 2 : result;
}

/* A literal sequence outside alternation, or inside a mandatory child, is a
   safe block prefilter. Captures delimit runs; counted nodes are never copied. */
static fn rx_required(rx_compiler *c, p16 first, rx_hints *hints)
{
        if (++c->depth > RX_PARSE_MAX)
                c->broken = true;
        for (p16 at = first; at && !c->broken;)
        {
                rx_node *node = c->pool->nodes + at;
                p16 from = at;
                positive length = 0;
                while (at && c->pool->nodes[at].kind == RX_BYTE)
                {
                        length++;
                        at = c->pool->nodes[at].next;
                }
                if (length)
                {
                        if (length > RX_LITERAL_MAX)
                                length = RX_LITERAL_MAX;
                        if (length > hints->literal_length)
                        {
                                hints->literal_length = length;
                                for (positive i = 0; i < length; i++)
                                {
                                        hints->literal[i] = c->pool->nodes[from].argument;
                                        from = c->pool->nodes[from].next;
                                }
                        }
                        continue;
                }
                if (node->kind == RX_CAPTURE || (node->kind == RX_COUNT && node->minimum > 0))
                        rx_required(c, node->left, hints);
                at = node->next;
        }
        c->depth--;
}

/* Compile above the current mark. Neither a failed compile nor its scratch
   metadata changes a published descriptor or the pool's ownership cursor. */
static bool rx_compile(rx_pool *pool, regex_program *out, string_address pattern,
                       bool extended, bool icase, bool escapes, p8 policy)
{
        rx_compiler c = {.pool = pool, .cursor = pool->used, .pattern = pattern,
                         .length = string_length(pattern), .extended = extended, .escapes = escapes};
        if (c.cursor.hints == RX_HINT_MAX)
                return false;
        if (!c.cursor.nodes)
                c.cursor.nodes = 1;
        p16 first = c.cursor.nodes;
        rx_hints *hints = pool->hints + c.cursor.hints++;
        memory_fill(hints, 0, sizeof(*hints));
        c.program = (regex_program){.nodes = pool->nodes, .sets = (const p8 (*)[256])pool->sets,
                                .hints = hints, .policy = policy, .flags = icase ? RX_IGNORE_CASE : 0};
        rx_fragment root = rx_alternation(address_of c);
        c.program.first = root.first;
        if (c.at != c.length || c.broken)
                return false;
        bool literal = root.first && c.cursor.nodes - first <= RX_LITERAL_MAX;
        for (p16 i = first; i < c.cursor.nodes; i++)
        {
                const rx_node *node = pool->nodes + i;
                if (node->kind != RX_BYTE)
                        literal = false;
                if (node->kind == RX_BACKREF)
                        c.program.flags |= RX_HAS_BACKREF;
                if (node->kind == RX_ALT || (node->kind == RX_COUNT && node->minimum != node->maximum))
                        c.program.flags |= RX_BRANCHING;
        }
        if (!rx_edges(address_of c, root.first, root.last, hints->first_skip, false))
        {
                c.program.flags |= RX_FIRST_KNOWN;
                for (b32 i = 0; i < 256; i++)
                        hints->first_skip[i] = !hints->first_skip[i];
        }
        if (!(c.program.flags & RX_HAS_BACKREF))
        {
                rx_edges(address_of c, root.first, root.last, hints->last_bytes, true);
                c.program.flags |= RX_LAST_KNOWN;
        }
        if (root.first && pool->nodes[root.first].kind == RX_BEGIN)
                c.program.flags |= RX_ANCHORED;
        rx_required(address_of c, root.first, hints);
        if (literal)
                c.program.flags |= RX_LITERAL_PROVES;
        hints->literal_anchors = memory_search_prepare(hints->literal, hints->literal_length, icase);
        if (c.broken)
                return false;
        *out = c.program;
        pool->used = c.cursor;
        return true;
}

/* Iterative graph execution. Continuations stay immutable while a choice can
   revisit them; a choice restores both its frame mark and capture undo mark. */
typedef struct {
        /* Zero resumes a sequence, a positive slot closes its capture,
           and -1 resumes a counted child. The slot is also the frame tag. */
        b16 slot;
        p16 node;
        p32 parent;
        positive repetitions, previous_position;
} rx_frame;

typedef struct {
        p16 node;
        p32 continuation, frame_mark, undo_mark;
        positive position, lower;
} rx_choice;

typedef struct {
        p8 slot;
        positive previous;
} rx_undo;

typedef struct {
        rx_frame *frames;
        rx_choice *choices;
        rx_undo *undo;
        p32 frame_capacity, choice_capacity, undo_capacity;
        const regex_program *program;
        string_address bytes;
        positive length, slots[20], best_slots[20];
        positive best_stop, best_limit, work_used, work_limit;
        p32 frame_used, choice_used, undo_used;
        p8 selection, active_captures;
        bool pending_exhaustion, first_exhausted;
} rx_match;

static p32 rx_frame_put(rx_match *match, rx_frame frame)
{
        if (match->frame_used == match->frame_capacity)
        {
                match->pending_exhaustion = true;
                return 0;
        }
        match->frames[match->frame_used++] = frame;
        return match->frame_used;
}

static bool rx_choice_put(rx_match *match, p16 node, p32 continuation,
                          positive position, positive lower)
{
        if (match->choice_used == match->choice_capacity)
        {
                match->pending_exhaustion = true;
                return false;
        }
        match->choices[match->choice_used++] = (rx_choice){
            node, continuation, match->frame_used, match->undo_used,
            position, lower};
        return true;
}

static bool rx_slot_put(rx_match *match, p8 slot, positive value)
{
        if (slot >= match->active_captures)
                return true;
        if (match->choice_used)
        {
                if (match->undo_used == match->undo_capacity)
                {
                        match->pending_exhaustion = true;
                        return false;
                }
                match->undo[match->undo_used++] =
                    (rx_undo){slot, match->slots[slot]};
        }
        match->slots[slot] = value;
        return true;
}

static bool rx_single(const regex_program *program, const rx_node *node, p8 byte)
{
        if (node->kind == RX_ANY)
                return (program->policy & REGEX_DOT_NEWLINE) || byte != '\n';
        if (node->kind == RX_SET)
                return program->sets[node->argument][byte];
        return ((program->flags & RX_IGNORE_CASE) ? byte_to_lower(byte) : byte) == node->argument;
}

static bool rx_accept(rx_match *match, positive position)
{
        const regex_program *program = match->program;
        if (match->selection == REGEX_FIRST || match->best_stop == positive_max)
                match->first_exhausted = match->pending_exhaustion;
        if (match->selection == REGEX_FIRST ||
            (match->selection == REGEX_EXACT_LONGEST && match->first_exhausted))
                return true;
        positive stop = position + (program->boundary == REGEX_BOUNDARY_WORD &&
                                    position < match->length);
        if (match->best_stop == positive_max)
        {
                match->best_limit = match->length;
                positive least = position + (!position && program->boundary == REGEX_BOUNDARY_WORD);
                while (match->best_limit > least && (program->flags & RX_LAST_KNOWN) &&
                       !program->hints->last_bytes[match->bytes[match->best_limit - 1]])
                        match->best_limit--;
                match->best_limit += program->boundary == REGEX_BOUNDARY_WORD &&
                                     match->best_limit < match->length;
        }
        if (match->best_stop == positive_max || stop > match->best_stop)
        {
                match->best_stop = stop;
                memory_copy_apart(match->best_slots, match->slots,
                                  match->active_captures * sizeof(positive));
        }
        return stop == match->best_limit;
}

static bool rx_run(rx_match *match, positive start)
{
        const regex_program *program = match->program;
        string_address bytes = match->bytes;
        positive length = match->length;
        p16 node = program->first;
        p32 continuation = 0;
        positive position = start, repetitions = 0, previous = positive_max;
        positive work = match->work_used, work_limit = match->work_limit;
        bool accepted = false;
        match->frame_used = match->choice_used = match->undo_used = 0;
        memory_fill(match->slots, -1, match->active_captures * sizeof(positive));
        if (!rx_slot_put(match, 0, start))
                return false;
        for (;;)
        {
                if (work == work_limit)
                {
                        match->pending_exhaustion = true;
                        goto backtrack;
                }
                work++;
                if (!node)
                {
                        if (continuation)
                        {
                                rx_frame frame = match->frames[continuation - 1];
                                p32 protected = match->choice_used
                                    ? match->choices[match->choice_used - 1].frame_mark : 0;
                                if (continuation == match->frame_used && continuation > protected)
                                        match->frame_used--;
                                continuation = frame.parent;
                                node = frame.node;
                                if (frame.slot > 0 &&
                                    !rx_slot_put(match, (p8)frame.slot, position))
                                        goto backtrack;
                                if (frame.slot < 0)
                                {
                                        repetitions = frame.repetitions;
                                        previous = frame.previous_position;
                                        goto count;
                                }
                                continue;
                        }
                        if ((program->boundary == REGEX_BOUNDARY_LINE && position != length) ||
                            (program->boundary == REGEX_BOUNDARY_WORD && position < length &&
                             string_set_name[bytes[position]]))
                                goto backtrack;
                        if (!rx_slot_put(match, 1, position))
                                goto backtrack;
                        if (rx_accept(match, position))
                        {
                                accepted = true;
                                break;
                        }
                        goto backtrack;
                }

                const rx_node *instruction = program->nodes + node;
                switch (instruction->kind)
                {
                case RX_BYTE:
                case RX_ANY:
                case RX_SET:
                        if (position >= length || !rx_single(program, instruction, bytes[position]))
                                goto backtrack;
                        position++;
                        break;
                case RX_BEGIN:
                        if (position && (!(program->policy & REGEX_LINE_ANCHORS) ||
                                         bytes[position - 1] != '\n'))
                                goto backtrack;
                        break;
                case RX_END:
                        if (position != length && (!(program->policy & REGEX_LINE_ANCHORS) ||
                                                         bytes[position] != '\n'))
                                goto backtrack;
                        break;
                case RX_EDGE:
                {
                        /* Bit (before*2 + after), in WORD/NOT_WORD/START/STOP order. */
                        static const p8 edge_masks[] = {6, 9, 2, 4};
                        bool before = position && string_set_name[bytes[position - 1]];
                        bool after = position < length && string_set_name[bytes[position]];
                        if (!(edge_masks[instruction->argument] & (1u << (before * 2 + after))))
                                goto backtrack;
                        break;
                }
                case RX_BACKREF:
                {
                        if (instruction->argument > program->groups)
                                goto backtrack;
                        positive from = match->slots[instruction->argument * 2];
                        positive to = match->slots[instruction->argument * 2 + 1];
                        if (from == positive_max || to == positive_max || to < from ||
                            to - from > length - position)
                                goto backtrack;
                        bipolar different = (program->flags & RX_IGNORE_CASE)
                            ? memory_compare_ascii_case(bytes + from, bytes + position, to - from)
                            : memory_compare(bytes + from, bytes + position, to - from);
                        if (different)
                                goto backtrack;
                        position += to - from;
                        break;
                }
                case RX_CAPTURE:
                case RX_ALT:
                {
                        p32 resume = continuation;
                        p8 slot = (p8)(instruction->argument * 2);
                        bool capture = instruction->kind == RX_CAPTURE && slot < match->active_captures;
                        if (capture && !rx_slot_put(match, slot, position))
                                goto backtrack;
                        if (capture || instruction->next)
                        {
                                resume = rx_frame_put(match, (rx_frame){
                                    capture ? slot + 1 : 0,
                                    instruction->next, continuation, 0, 0});
                                if (!resume)
                                        goto backtrack;
                        }
                        if (instruction->kind == RX_ALT &&
                            !rx_choice_put(match, instruction->right, resume, position, positive_max))
                                goto backtrack;
                        continuation = resume;
                        node = instruction->left;
                        continue;
                }
                case RX_COUNT:
                {
                        const rx_node *child = program->nodes + instruction->left;
                        if (instruction->left && !child->next &&
                            (child->kind == RX_BYTE || child->kind == RX_ANY || child->kind == RX_SET))
                        {
                                positive taken = 0, limit = length - position;
                                if (instruction->maximum >= 0 && limit > (positive)instruction->maximum)
                                        limit = instruction->maximum;
                                positive available = work_limit - work;
                                if (available > limit)
                                        available = limit;
                                string_address at = bytes + position;
                                if (child->kind == RX_BYTE && !(program->flags & RX_IGNORE_CASE))
                                        taken = memory_span_byte(at, child->argument, available);
                                else if (child->kind == RX_ANY)
                                {
                                        string_address newline = (program->policy & REGEX_DOT_NEWLINE)
                                            ? 0 : memory_first_of(at, '\n', available);
                                        taken = newline ? (positive)(newline - at) : available;
                                }
                                else if (child->kind == RX_SET)
                                        taken = string_span_max(at, available,
                                            (const b8 *)program->sets[child->argument]);
                                else
                                        while (taken < available && rx_single(program, child, at[taken]))
                                                taken++;
                                work += taken;
                                /* The old loop charged only matching bytes: a mismatch at
                                   the work limit must still finish without exhaustion. */
                                if (taken == available && taken < limit && rx_single(program, child, at[taken]))
                                {
                                        match->pending_exhaustion = true;
                                        goto backtrack;
                                }
                                if (taken < (positive)instruction->minimum)
                                        goto backtrack;
                                if (taken > (positive)instruction->minimum &&
                                    !rx_choice_put(match, instruction->next, continuation,
                                                   position + taken - 1, position + instruction->minimum))
                                        goto backtrack;
                                position += taken;
                                break;
                        }
                        repetitions = 0;
                        previous = positive_max;
                        goto count;
                }
                }
                node = instruction->next;
                continue;

        count:
                instruction = program->nodes + node;
                if ((repetitions >= (positive)instruction->minimum && instruction->maximum >= 0 &&
                     repetitions >= (positive)instruction->maximum) ||
                    (instruction->maximum < 0 && repetitions > (positive)instruction->minimum && previous == position))
                {
                        node = instruction->next;
                        continue;
                }
                if (repetitions >= (positive)instruction->minimum &&
                    !rx_choice_put(match, instruction->next, continuation, position, positive_max))
                        goto backtrack;
                continuation = rx_frame_put(match, (rx_frame){
                    -1, node, continuation, repetitions + 1, position});
                if (!continuation)
                        goto backtrack;
                node = instruction->left;
                continue;

        backtrack:
                if (!match->choice_used)
                        break;
                rx_choice choice = match->choices[--match->choice_used];
                while (match->undo_used > choice.undo_mark)
                {
                        rx_undo undo = match->undo[--match->undo_used];
                        match->slots[undo.slot] = undo.previous;
                }
                match->frame_used = choice.frame_mark;
                node = choice.node;
                continuation = choice.continuation;
                position = choice.position;
                if (choice.lower != positive_max && choice.position > choice.lower)
                {
                        choice.position--;
                        match->choices[match->choice_used++] = choice;
                }
        }
        match->work_used = work;
        return accepted;
}

static p8 rx_find(rx_match *match, const regex_program *program, p8 mode, bool captures,
                   string_address bytes, positive length, positive start)
{
        match->program = program;
        match->bytes = bytes;
        match->length = length;
        match->work_used = 0;
        match->selection = (program->flags & RX_BRANCHING) || program->boundary == REGEX_BOUNDARY_WORD
                               ? mode : REGEX_FIRST;
        match->active_captures = captures || (program->flags & RX_HAS_BACKREF)
                                    ? (p8)((program->groups + 1) * 2) : 0;
        if (start > length)
                return RX_NO_MATCH;
        if ((program->flags & RX_LITERAL_PROVES) && !program->boundary && mode != REGEX_EXACT_LONGEST)
        {
                const rx_hints *hints = program->hints;
                string_address found = text_literal_find(bytes, length, start,
                    (string_address)hints->literal, hints->literal_length,
                    (program->flags & RX_IGNORE_CASE) != 0, hints->literal_anchors);
                if (!found)
                        return RX_NO_MATCH;
                memory_fill(match->slots, -1, match->active_captures * sizeof(positive));
                match->slots[0] = (positive)(found - bytes);
                match->slots[1] = match->slots[0] + hints->literal_length;
                return RX_MATCH;
        }
        for (positive at = start; at <= length; at++)
        {
                if (program->boundary == REGEX_BOUNDARY_WORD && at && mode != REGEX_EXACT_LONGEST)
                        at += string_span_max(bytes + at, length - at, string_set_name);
                if ((program->flags & RX_FIRST_KNOWN) && !program->boundary && mode != REGEX_EXACT_LONGEST)
                {
                        at += string_span_max(bytes + at, length - at, (const b8 *)program->hints->first_skip);
                        if (at == length)
                                return RX_NO_MATCH;
                }
                match->best_stop = positive_max;
                bool found = false;
                if (program->boundary == REGEX_BOUNDARY_NONE || !at)
                        found = rx_run(match, at);
                if (!found && program->boundary == REGEX_BOUNDARY_WORD && at < length && !string_set_name[bytes[at]])
                        found = rx_run(match, at + 1);
                if (match->best_stop != positive_max)
                {
                        memory_copy_apart(match->slots, match->best_slots, match->active_captures * sizeof(positive));
                        found = true;
                }
                if (found && (mode != REGEX_EXACT_LONGEST || !match->first_exhausted))
                        return RX_MATCH;
                if (!found && mode == REGEX_EXACT_LONGEST)
                        return RX_NO_MATCH;
                if (match->pending_exhaustion)
                {
                        match->pending_exhaustion = false;
                        return RX_COMPLEX;
                }
                if (program->boundary == REGEX_BOUNDARY_LINE ||
                    ((program->flags & RX_ANCHORED) && program->boundary != REGEX_BOUNDARY_WORD))
                        return RX_NO_MATCH;
        }
        return RX_NO_MATCH;
}

#define REGEX_SCRATCH_MAX 20000

static rx_pool regex_pool;
static rx_mark regex_retained;
static regex_program regex_current;
static rx_frame regex_frames[REGEX_SCRATCH_MAX];
static rx_choice regex_choices[REGEX_SCRATCH_MAX];
static rx_undo regex_undo[REGEX_SCRATCH_MAX];
static rx_match regex_match = {
    .frames = regex_frames, .choices = regex_choices, .undo = regex_undo,
    .frame_capacity = REGEX_SCRATCH_MAX, .choice_capacity = REGEX_SCRATCH_MAX,
    .undo_capacity = REGEX_SCRATCH_MAX,
    .work_limit = 100000000,
};
static bool regex_demand = true;

#define regex_slots regex_match.slots
#define regex_group_count regex_current.groups
#define regex_boundary regex_current.boundary

/* Ordinary compilations borrow the space above the retained program mark. */
static bool regex_compile(string_address pattern, bool extended, bool icase,
                          bool escapes, p8 policy)
{
        regex_pool.used = regex_retained;
        regex_demand = true;
        return rx_compile(&regex_pool, &regex_current, pattern, extended,
                          icase, escapes, policy);
}

static fn regex_keep(regex_program *into)
{
        *into = regex_current;
        regex_retained = regex_pool.used;
}

static bool regex_find(p8 mode, string_address text, positive length, positive from)
{
        p8 result = rx_find(&regex_match, &regex_current, mode, regex_demand,
                            text, length, from);
        if (result == RX_COMPLEX)
        {
                text_error(null, "regular expression too complex");
                text_status = 2;
        }
        return result == RX_MATCH;
}
