#include "nurikabe.h"

string format_time(const steady_clock::time_point start, const steady_clock::time_point finish) {
    ostringstream oss;

    if (finish - start < std::chrono::microseconds(1)) {
        oss << duration_cast<duration<double, micro>>(finish - start).count() << " microseconds";
    } else if (finish - start < std::chrono::seconds(1)) {
        oss << duration_cast<duration<double, milli>>(finish - start).count() << " milliseconds";
    } else {
        oss << duration_cast<duration<double>>(finish - start).count() << " seconds";
    }

    return oss.str();
}

Grid::Grid(const int width, const int height, const string& s)
    : m_width(width), m_height(height), m_total_black(width * height),
    m_cells(), m_regions(), m_sitrep(KEEP_GOING), m_output(), m_prng(1729) {

    // Validate width and height.

    if (width < 1) {
        throw runtime_error("RUNTIME ERROR: Grid::Grid() - width must be at least 1.");
    }

    if (height < 1) {
        throw runtime_error("RUNTIME ERROR: Grid::Grid() - height must be at least 1.");
    }

    // Initialize m_cells. We must set everything to UNKNOWN before calling add_region() below.

    m_cells.resize(width, vector<pair<State, shared_ptr<Region>>>(
        height, make_pair(UNKNOWN, shared_ptr<Region>())));

    // Parse the string.

    vector<int> v;

    const regex r(R"((\d+)|( )|(\n)|[^\d \n])");

    for (sregex_iterator i(s.begin(), s.end(), r), end; i != end; ++i) {
        const smatch& m = *i;

        if (m[1].matched) {
            v.push_back(stoi(m[1]));
        } else if (m[2].matched) {
            v.push_back(0);
        } else if (m[3].matched) {
            // Do nothing.
        } else {
            throw runtime_error("RUNTIME ERROR: Grid::Grid() - "
                "s must contain only digits, spaces, and newlines.");
        }
    }

    // Validate the number of cells. Note that we can't do this before
    // parsing, because numbers 10 and above occupy multiple characters.

    if (v.size() != static_cast<size_t>(width * height)) {
        throw runtime_error("RUNTIME ERROR: Grid::Grid() - "
            "s must contain width * height numbers and spaces.");
    }

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            const int n = v[x + y * width];

            if (n > 0) {
                // Testing x - 1 and x + 1 is unnecessary because
                // horizontally adjacent numbers would have been concatenated.
                // Testing y + 1 is unnecessary because if there are
                // vertically adjacent numbers, one's above and one's below.

                if (valid(x, y - 1) && cell(x, y - 1) > 0) {
                    throw runtime_error("RUNTIME ERROR: Grid::Grid() - "
                        "s contains vertically adjacent numbers.");
                }

                // Set the cell's state and region.

                cell(x, y) = static_cast<State>(n);

                add_region(x, y);

                // m_total_black is width * height - (sum of all numbered cells).
                // Calculating this allows us to determine whether black regions
                // are partial or complete with a simple size test.
                // Humans are capable of this but it's not convenient for them.

                m_total_black -= n;
            }
        }
    }

    print("I'm okay to go!");
}

Grid::SitRep Grid::solve(const bool verbose, const bool guessing) {
    cache_map_t cache;


    // See if we're done. Before declaring victory, look for contradictions.

    if (known() == m_width * m_height) {
        if (detect_contradictions(verbose, cache)) {
            return CONTRADICTION_FOUND;
        }

        if (verbose) {
            print("I'm done!");
        }

        return SOLUTION_FOUND;
    }


    // Run increasingly expensive steps of analysis.
    // Return as soon as one succeeds.

    // Running detect_contradictions() just before analyze_confinement() increases performance.
    // detect_contradictions() is moderately expensive, so it should be run after extremely cheap
    // steps of analysis. However, detect_contradictions() accelerates analyze_confinement(),
    // which is even more expensive. This doesn't affect correctness:
    // * We always run detect_contradictions() before declaring victory.
    // * The other steps of analysis are robust; if they attempt to fuse two numbered regions
    // or mark an already known cell, they bail out early.
    // * analyze_confinement() can still assume that it has a cache.

    if (analyze_complete_islands(verbose)
        || analyze_single_liberties(verbose)
        || analyze_dual_liberties(verbose)
        || analyze_unreachable_cells(verbose)
        || analyze_potential_pools(verbose)
        || detect_contradictions(verbose, cache)
        || analyze_confinement(verbose, cache)
        || (guessing && analyze_hypotheticals(verbose))) {

        return m_sitrep;
    }


    if (verbose) {
        print("I'm stumped!");
    }

    return CANNOT_PROCEED;
}

// Look for complete islands.
bool Grid::analyze_complete_islands(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        const Region& r = **i;

        if (r.numbered() && r.size() == r.number()) {
            mark_as_black.insert(r.unk_begin(), r.unk_end());
        }
    }

    return process(verbose, mark_as_black, mark_as_white, "Complete islands found.");
}

// Look for partial regions that can expand into only one cell. They must expand.
bool Grid::analyze_single_liberties(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        const Region& r = **i;

        const bool partial =
            (r.black() && r.size() < m_total_black)
                || r.white()
                || (r.numbered() && r.size() < r.number());

        if (partial && r.unk_size() == 1) {
            if (r.black()) {
                mark_as_black.insert(*r.unk_begin());
            } else {
                mark_as_white.insert(*r.unk_begin());
            }
        }
    }

    return process(verbose, mark_as_black, mark_as_white,
        "Expanded partial regions with only one liberty.");
}

// Look for N - 1 islands with exactly two diagonal liberties.
bool Grid::analyze_dual_liberties(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        const Region& r = **i;

        if (r.numbered() && r.size() == r.number() - 1 && r.unk_size() == 2) {
            const int x1 = r.unk_begin()->first;
            const int y1 = r.unk_begin()->second;
            const int x2 = next(r.unk_begin())->first;
            const int y2 = next(r.unk_begin())->second;

            if (abs(x1 - x2) == 1 && abs(y1 - y2) == 1) {
                pair<int, int> p;

                if (r.contains(x1, y2)) {
                    p = make_pair(x2, y1);
                } else {
                    p = make_pair(x1, y2);
                }

                // The far cell might already be black, in which case there's nothing to do.
                // It could even be white/numbered (if it's part of this island), in which case
                // there's still nothing to do.
                // (If it's white/numbered and not part of this island,
                // we'll eventually detect a contradiction.)

                if (cell(p.first, p.second) == UNKNOWN) {
                    mark_as_black.insert(p);
                }
            }
        }
    }

    return process(verbose, mark_as_black, mark_as_white,
        "N - 1 islands with exactly two diagonal liberties found.");
}

// Look for unreachable cells. They must be black.
// This supersedes complete island analysis and forbidden bridge analysis.
// (We run complete island analysis above because it's fast
// and it makes the output easier to understand.)
bool Grid::analyze_unreachable_cells(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (int x = 0; x < m_width; ++x) {
        for (int y = 0; y < m_height; ++y) {
            if (unreachable(x, y)) {
                mark_as_black.insert(make_pair(x, y));
            }
        }
    }

    return process(verbose, mark_as_black, mark_as_white, "Unreachable cells blackened.");
}

// Look for squares of one unknown and three black cells, or two unknown and two black cells.
bool Grid::analyze_potential_pools(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (int x = 0; x < m_width - 1; ++x) {
        for (int y = 0; y < m_height - 1; ++y) {
            struct XYState {
                int x;
                int y;
                State state;
            };

            array<XYState, 4> a = { {
                { x, y, cell(x, y) },
                { x + 1, y, cell(x + 1, y) },
                { x, y + 1, cell(x, y + 1) },
                { x + 1, y + 1, cell(x + 1, y + 1) }
            } };

            static_assert(UNKNOWN < BLACK, "This code assumes that UNKNOWN < BLACK.");

            sort(a.begin(), a.end(), [](const XYState& l, const XYState& r) {
                return l.state < r.state;
            });

            if (a[0].state == UNKNOWN
                && a[1].state == BLACK
                && a[2].state == BLACK
                && a[3].state == BLACK) {

                mark_as_white.insert(make_pair(a[0].x, a[0].y));

            } else if (a[0].state == UNKNOWN
                && a[1].state == UNKNOWN
                && a[2].state == BLACK
                && a[3].state == BLACK) {

                for (int i = 0; i < 2; ++i) {
                    set<pair<int, int>> imagine_black;

                    imagine_black.insert(make_pair(a[0].x, a[0].y));

                    if (unreachable(a[1].x, a[1].y, imagine_black)) {
                        mark_as_white.insert(make_pair(a[0].x, a[0].y));
                    }

                    std::swap(a[0], a[1]);
                }
            }
        }
    }

    return process(verbose, mark_as_black, mark_as_white, "Whitened cells to prevent pools.");
}

// A region would be "confined" if it could not be completed.
// Black regions need to consume m_total_black cells.
// White regions need to escape to a number.
// Numbered regions need to consume N cells.

// Confinement analysis consists of imagining what would happen if a particular unknown cell
// were black or white. If that would cause any region to be confined, the unknown cell
// must be the opposite color.

// Black cells can't confine black regions, obviously.
// Black cells can confine white regions, by isolating them.
// Black cells can confine numbered regions, by confining them to an insufficiently large space.

// White cells can confine black regions, by confining them to an insufficiently large space.
//   (Humans look for isolation here, i.e. permanently separated black regions.
//   That's harder for us to detect, but counting cells is similarly powerful.)
// White cells can't confine white regions.
//   (This is true for freestanding white cells, white cells added to other white regions,
//   and white cells added to numbered regions.)
// White cells can confine numbered regions, when added to other numbered regions.
//   This is the most complicated case to analyze. For example:
//   ####3
//   #6 xXx
//   #.  x
//   ######
//   Imagining cell 'X' to be white additionally prevents region 6 from consuming
//   three 'x' cells. (This is true regardless of what other cells region 3 would
//   eventually occupy.)
bool Grid::analyze_confinement(const bool verbose, cache_map_t& cache) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    for (int x = 0; x < m_width; ++x) {
        for (int y = 0; y < m_height; ++y) {
            if (cell(x, y) == UNKNOWN) {
                set<pair<int, int>> verboten;
                verboten.insert(make_pair(x, y));

                for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
                    const Region& r = **i;

                    if (confined(*i, cache, verboten)) {
                        if (r.black()) {
                            mark_as_black.insert(make_pair(x, y));
                        } else {
                            mark_as_white.insert(make_pair(x, y));
                        }
                    }
                }
            }
        }
    }

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        const Region& r = **i;

        if (r.numbered() && r.size() < r.number()) {
            for (auto u = r.unk_begin(); u != r.unk_end(); ++u) {
                set<pair<int, int>> verboten;
                verboten.insert(*u);

                insert_valid_unknown_neighbors(verboten, u->first, u->second);

                for (auto k = m_regions.begin(); k != m_regions.end(); ++k) {
                    if (k != i && (*k)->numbered() && confined(*k, cache, verboten)) {
                        mark_as_black.insert(*u);
                    }
                }
            }
        }
    }

    return process(verbose, mark_as_black, mark_as_white, "Confinement analysis succeeded.");
}

// Guess cells in a deterministic but pseudorandomized order.
// This attempts to avoid repeatedly guessing cells that won't get us anywhere.
// Prioritize guesses near white cells, which appears to be an especially good heuristic.
// (In particular, it appears to be better than prioritizing guesses near white regions.)
// Manhattan distance appears to work well; see https://en.wikipedia.org/wiki/Taxicab_geometry
vector<pair<int, int>> Grid::guessing_order() {
    // Find all unknown cells and all white cells.
    // The greatest possible Manhattan distance on the grid is m_width - 1 + m_height - 1,
    // so we use m_width + m_height as an extremely large placeholder.

    vector<tuple<int, int, int>> x_y_manhattan;
    vector<pair<int, int>> white_cells;

    for (int x = 0; x < m_width; ++x) {
        for (int y = 0; y < m_height; ++y) {
            switch (cell(x, y)) {
                case UNKNOWN:
                    x_y_manhattan.push_back(make_tuple(x, y, m_width + m_height));
                    break;
                case WHITE:
                    white_cells.push_back(make_pair(x, y));
                    break;
                default:
                    break;
            }
        }
    }


    // Randomly shuffle the unknown cells.

    shuffle(x_y_manhattan.begin(), x_y_manhattan.end(), m_prng);


    // Determine the Manhattan distance from each unknown cell to the nearest white cell.
    // There's probably a cleverer algorithm for this.

    for (auto& [ x1, y1, manhattan ] : x_y_manhattan) {
        for (const auto& [ x2, y2 ] : white_cells) {
            manhattan = min(manhattan, abs(x1 - x2) + abs(y1 - y2));
        }
    }


    // Prioritize the unknown cells by the Manhattan distance to the nearest white cell.
    // stable_sort() avoids disrupting the random_shuffle() above.

    stable_sort(x_y_manhattan.begin(), x_y_manhattan.end(),
        [](const tuple<int, int, int>& l, const tuple<int, int, int>& r) {
            return get<2>(l) < get<2>(r);
        });


    vector<pair<int, int>> ret;

    transform(x_y_manhattan.begin(), x_y_manhattan.end(), back_inserter(ret),
        [](const tuple<int, int, int>& t) { return make_pair(get<0>(t), get<1>(t)); });

    return ret;
}

bool Grid::analyze_hypotheticals(const bool verbose) {
    set<pair<int, int>> mark_as_black;
    set<pair<int, int>> mark_as_white;

    const vector<pair<int, int>> v = guessing_order();

    int failed_guesses = 0;
    set<pair<int, int>> failed_coords;

    for (const auto& [ x, y ] : v) {
        for (int i = 0; i < 2; ++i) {
            const State color = i == 0 ? BLACK : WHITE;
            auto& mark_as_diff = i == 0 ? mark_as_white : mark_as_black;
            auto& mark_as_same = i == 0 ? mark_as_black : mark_as_white;

            Grid other(*this);

            other.mark(color, x, y);

            SitRep sr = KEEP_GOING;

            while (sr == KEEP_GOING) {
                sr = other.solve(false, false);
            }

            if (sr == CONTRADICTION_FOUND) {
                mark_as_diff.insert(make_pair(x, y));
                return process(verbose, mark_as_black, mark_as_white,
                    "Hypothetical contradiction found.", failed_guesses, failed_coords);
            }

            if (sr == SOLUTION_FOUND) {
                mark_as_same.insert(make_pair(x, y));
                return process(verbose, mark_as_black, mark_as_white,
                    "Hypothetical solution found.", failed_guesses, failed_coords);
            }

            // sr == CANNOT_PROCEED
            ++failed_guesses;
            failed_coords.insert(make_pair(x, y));
        }
    }

    return false;
}

int Grid::known() const {
    int ret = 0;

    for (int x = 0; x < m_width; ++x) {
        for (int y = 0; y < m_height; ++y) {
            if (cell(x, y) != UNKNOWN) {
                ++ret;
            }
        }
    }

    return ret;
}

vector<vector<int> > Grid::getFinal() const {
    const auto& [s,v,updated,ctr,failed_guesses,failed_coords] = m_output[m_output.size()-1];
    vector<vector<int> > out;
    for (int y = 0; y < m_height; ++y) {
        out.push_back(vector<int>());
        for (int x = 0; x < m_width; ++x) {
            switch (v[x][y]) {
                case UNKNOWN: out[y].push_back(-3); break;
                case WHITE:   out[y].push_back(-2); break;
                case BLACK:   out[y].push_back(-1); break;
                default:      out[y].push_back(v[x][y]); break;
            }
        }
    }
    return out;
}

void Grid::write(ostream& os, const steady_clock::time_point start, const steady_clock::time_point finish) const {
    os <<
R"(<!DOCTYPE html>
<html lang="en">
  <head>
    <meta http-equiv="Content-Type" content="text/html;charset=utf-8" />
    <style>
      body {
        font-family: Verdana, sans-serif;
        line-height: 1.4;
      }
      table {
        border: solid 3px #000000;
        border-collapse: collapse;
      }
      td {
        border: solid 1px #000000;
        text-align: center;
        width: 20px;
        height: 20px;
      }
      td.unknown   { background-color: #C0C0C0; }
      td.white.new { background-color: #FFFF00; }
      td.white.old { }
      td.black.new { background-color: #008080; }
      td.black.old { background-color: #808080; }
      td.number    { }
      td.failed    { border: solid 3px #000000; }
    </style>
    <title>Nurikabe</title>
  </head>
  <body>
)";

    steady_clock::time_point old_ctr = start;

    for (const auto& [ s, v, updated, ctr, failed_guesses, failed_coords ] : m_output) {
        os << s << " (" << format_time(old_ctr, ctr) << ")\n";

        if (failed_guesses == 1) {
            os << "<br/>1 guess failed.\n";
        } else if (failed_guesses > 0) {
            os << "<br/>" << failed_guesses << " guesses failed.\n";
        }

        old_ctr = ctr;

        os << "<table>\n";

        for (int y = 0; y < m_height; ++y) {
            os << "<tr>";

            for (int x = 0; x < m_width; ++x) {
                os << "<td class=\"";
                os << (updated.find(make_pair(x, y)) != updated.end() ? "new " : "old ");

                if (failed_coords.find(make_pair(x, y)) != failed_coords.end()) {
                    os << "failed ";
                }

                switch (v[x][y]) {
                    case UNKNOWN: os << "unknown\"> ";           break;
                    case WHITE:   os <<   "white\">.";           break;
                    case BLACK:   os <<   "black\">#";           break;
                    default:      os <<  "number\">" << v[x][y]; break;
                }

                os << "</td>";
            }

            os << "</tr>\n";
        }

        os << "</table><br/>\n";
    }
    os << "Total: " << format_time(start, finish) << "\n";

    os <<
        "  </body>\n"
        "</html>\n";
}

bool Grid::valid(const int x, const int y) const {
    return x >= 0 && x < m_width && y >= 0 && y < m_height;
}

Grid::State& Grid::cell(const int x, const int y) {
    return m_cells[x][y].first;
}

const Grid::State& Grid::cell(const int x, const int y) const {
    return m_cells[x][y].first;
}

shared_ptr<Grid::Region>& Grid::region(const int x, const int y) {
    return m_cells[x][y].second;
}

const shared_ptr<Grid::Region>& Grid::region(const int x, const int y) const {
    return m_cells[x][y].second;
}

void Grid::print(const string& s, const set<pair<int, int>>& updated,
    const int failed_guesses, const set<pair<int, int>>& failed_coords) {
    vector<vector<State>> v(m_width, vector<State>(m_height));

    for (int x = 0; x < m_width; ++x) {
        for (int y = 0; y < m_height; ++y) {
            v[x][y] = cell(x, y);
        }
    }

    m_output.push_back(make_tuple(s, v, updated, steady_clock::now(), failed_guesses, failed_coords));
}

bool Grid::process(const bool verbose, const set<pair<int, int>>& mark_as_black,
    const set<pair<int, int>>& mark_as_white, const string& s,
    const int failed_guesses, const set<pair<int, int>>& failed_coords) {

    if (mark_as_black.empty() && mark_as_white.empty()) {
        return false;
    }

    for (const auto& [ x, y ] : mark_as_black) {
        mark(BLACK, x, y);
    }

    for (const auto& [ x, y ] : mark_as_white) {
        mark(WHITE, x, y);
    }

    if (verbose) {
        set<pair<int, int>> updated(mark_as_black);
        updated.insert(mark_as_white.begin(), mark_as_white.end());

        string t = s;

        if (m_sitrep == CONTRADICTION_FOUND) {
            t += " (Contradiction found! Attempted to fuse two numbered regions"
                " or mark an already known cell.)";
        }

        print(t, updated, failed_guesses, failed_coords);
    }

    return true;
}

template <typename F> void Grid::for_valid_neighbors(const int x, const int y, F f) const {
    if (x > 0) {
        f(x - 1, y);
    }

    if (x + 1 < m_width) {
        f(x + 1, y);
    }

    if (y > 0) {
        f(x, y - 1);
    }

    if (y + 1 < m_height) {
        f(x, y + 1);
    }
}

void Grid::insert_valid_neighbors(set<pair<int, int>>& s, const int x, const int y) const {
    for_valid_neighbors(x, y, [&](const int a, const int b) {
        s.insert(make_pair(a, b));
    });
}

void Grid::insert_valid_unknown_neighbors(set<pair<int, int>>& s, const int x, const int y) const {
    for_valid_neighbors(x, y, [&](const int a, const int b) {
        if (cell(a, b) == UNKNOWN) {
            s.insert(make_pair(a, b));
        }
    });
}

void Grid::add_region(const int x, const int y) {
    // Construct a region, then add it to the cell and the set of all regions.

    set<pair<int, int>> unknowns;
    insert_valid_unknown_neighbors(unknowns, x, y);

    auto r = make_shared<Region>(cell(x, y), x, y, unknowns);

    region(x, y) = r;

    m_regions.insert(r);
}

void Grid::mark(const State s, const int x, const int y) {
    if (s != WHITE && s != BLACK) {
        throw logic_error("LOGIC ERROR: Grid::mark() - s must be either WHITE or BLACK.");
    }

    // If we're asked to mark an already known cell, we've encountered a contradiction.
    // Remember this, so that solve() can report the contradiction.

    if (cell(x, y) != UNKNOWN) {
        m_sitrep = CONTRADICTION_FOUND;
        return;
    }

    // Set the cell's new state. Because it's now known,
    // update each region's set of surrounding unknown cells.

    cell(x, y) = s;

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        (*i)->unk_erase(x, y);
    }

    // Marking a cell as white or black could create an independent region,
    // could be added to an existing region, or could connect 2, 3, or 4 separate regions.
    // The easiest thing to do is to create a region for this cell,
    // and then fuse it to any adjacent compatible regions.

    add_region(x, y);

    // Don't attempt to cache these regions.
    // Each fusion could change this cell's region or its neighbors' regions.

    for_valid_neighbors(x, y, [this, x, y](const int a, const int b) {
        fuse_regions(region(x, y), region(a, b));
    });
}

// Note that r1 and r2 are passed by modifiable value. It's convenient to be able to swap them.
void Grid::fuse_regions(shared_ptr<Region> r1, shared_ptr<Region> r2) {

    // If we don't have two different regions, we're done.

    if (!r1 || !r2 || r1 == r2) {
        return;
    }

    // If we're asked to fuse two numbered regions, we've encountered a contradiction.
    // Remember this, so that solve() can report the contradiction.

    if (r1->numbered() && r2->numbered()) {
        m_sitrep = CONTRADICTION_FOUND;
        return;
    }

    // Black regions can't be fused with non-black regions.

    if (r1->black() != r2->black()) {
        return;
    }

    // We'll use r1 as the "primary" region, to which r2's cells are added.
    // It would be efficient to process as few cells as possible.
    // Therefore, we'd like to use the bigger region as the primary region.

    if (r2->size() > r1->size()) {
        std::swap(r1, r2);
    }

    // However, if the secondary region is numbered, then the primary region
    // must be white, so we need to swap them, even if the numbered region is smaller.

    if (r2->numbered()) {
        std::swap(r1, r2);
    }

    // Fuse the secondary region into the primary region.

    r1->insert(r2->begin(), r2->end());
    r1->unk_insert(r2->unk_begin(), r2->unk_end());

    // Update the secondary region's cells to point to the primary region.

    for (auto i = r2->begin(); i != r2->end(); ++i) {
        region(i->first, i->second) = r1;
    }

    // Erase the secondary region from the set of all regions.
    // When this function returns, the secondary region will be destroyed.

    m_regions.erase(r2);
}

bool Grid::impossibly_big_white_region(const int n) const {
    // A white region of N cells is impossibly big
    // if it could never be connected to any numbered region.

    return none_of(m_regions.begin(), m_regions.end(),
        [n](const shared_ptr<Region>& p) {
            // Add one because a bridge would be needed.
            return p->numbered() && p->size() + n + 1 <= p->number();
        });
}

// The cell at (x_root, y_root) is unreachable if it is unknown
// and it cannot be connected to a numbered region or a white region.
// An unknown cell completely surrounded by black cells is obviously unreachable,
// but because unreachability takes distance into account, it is considerably more powerful.
// We use a breadth-first search to discover the shortest path from the root to a numbered region
// or a white region. (Essentially, we're imagining a chain of white cells starting from the root.)
// We refuse to consider stepping anywhere that would join two numbered regions.
// Additionally, while we want to connect to a numbered region, we can't create a region that's
// too big for its number. (We'll add up the number of white cells that it took us to get from
// the root to the potential connection point, the current size of the numbered region,
// and the sizes of any white regions (up to 2) that would also be connected.)
// Finally, while we'd like to connect to a white region, we can't create
// an impossibly big white region.
// Note that this supersedes complete island analysis and forbidden bridge analysis.
// The "discovered" parameter can be used to tell unreachable() to avoid stepping on a cell.
// This is used when identifying squares of two unknown and two black cells;
// if imagining cell A to be black makes cell B unreachable, then cell A must be white.
bool Grid::unreachable(const int x_root, const int y_root, set<pair<int, int>> discovered) const {

    // We're interested in unknown cells.

    if (cell(x_root, y_root) != UNKNOWN) {
        return false;
    }

    // See https://en.wikipedia.org/wiki/Breadth-first_search

    queue<tuple<int, int, int>> q;

    q.push(make_tuple(x_root, y_root, 1));
    discovered.insert(make_pair(x_root, y_root));

    while (!q.empty()) {
        const auto [ x_curr, y_curr, n_curr ] = q.front();
        q.pop();

        set<shared_ptr<Region>> white_regions;
        set<shared_ptr<Region>> numbered_regions;

        for_valid_neighbors(x_curr, y_curr, [&](const int x, const int y) {
            const shared_ptr<Region>& r = region(x, y);

            if (r && r->white()) {
                white_regions.insert(r);
            } else if (r && r->numbered()) {
                numbered_regions.insert(r);
            }
        });

        int size = 0;

        for (auto i = white_regions.begin(); i != white_regions.end(); ++i) {
            size += (*i)->size();
        }

        for (auto i = numbered_regions.begin(); i != numbered_regions.end(); ++i) {
            size += (*i)->size();
        }

        if (numbered_regions.size() > 1) {
            continue;
        }

        if (numbered_regions.size() == 1) {
            const int num = (*numbered_regions.begin())->number();

            if (n_curr + size <= num) {
                return false;
            } else {
                continue;
            }
        }

        if (!white_regions.empty()) {
            if (impossibly_big_white_region(n_curr + size)) {
                continue;
            } else {
                return false;
            }
        }

        for_valid_neighbors(x_curr, y_curr, [&, n_curr = n_curr](const int x, const int y) {
            if (cell(x, y) == UNKNOWN && discovered.insert(make_pair(x, y)).second) {
                q.push(make_tuple(x, y, n_curr + 1));
            }
        });
    }

    return true;
}

namespace {
    // This explicitly specified underlying type significantly increases performance.
    enum Flag : unsigned char {
        NONE,
        OPEN,
        CLOSED,
        VERBOTEN
    };
}

// Is r confined, assuming that we can't consume verboten cells?
bool Grid::confined(const shared_ptr<Region>& r, cache_map_t& cache,
    const set<pair<int, int>>& verboten) const {

    // When we look for contradictions, we run confinement analysis (A) without verboten cells.
    // This gives us an opportunity to accelerate later confinement analysis (B)
    // when we have verboten cells.
    // During A, we'll record the unknown cells that we consumed.
    // During B, if none of the verboten cells are ones that we consumed,
    // then the verboten cells can't confine us.

    if (!verboten.empty()) {
        const auto i = cache.find(r);

        if (i == cache.end()) {
            return false; // We didn't consume any unknown cells.
        }

        const auto& consumed = i->second;

        if (none_of(verboten.begin(), verboten.end(), [&](const pair<int, int>& p) {
            return consumed.find(p) != consumed.end();
        })) {
            return false;
        }
    }


    vector<Flag> flags(m_width * m_height, NONE);

    // The open set contains cells that we're considering adding to the region.
    for (auto i = r->unk_begin(); i != r->unk_end(); ++i) {
        flags[i->first + i->second * m_width] = OPEN;
    }

    // The closed set contains cells that we've hypothetically added to the region.
    for (auto i = r->begin(); i != r->end(); ++i) {
        flags[i->first + i->second * m_width] = CLOSED;
    }

    int closed_size = r->size();

    // Flag the verboten cells last, because this could overwrite open flags.
    for (auto i = verboten.begin(); i != verboten.end(); ++i) {
        flags[i->first + i->second * m_width] = VERBOTEN;
    }

    // While we need to consume more cells...
    while ((r->black() && closed_size < m_total_black)
        || r->white()
        || (r->numbered() && closed_size < r->number())) {

        // Do we have a cell to consider?

        const auto iter = find(flags.begin(), flags.end(), OPEN);

        if (iter == flags.end()) {
            break; // We don't.
        }

        *iter = NONE;

        const int index = static_cast<int>(iter - flags.begin());

        const pair<int, int> p(index % m_width, index / m_width);

        // Consider cell p.

        // We need to compare our region r with p's region (if any).
        const auto& area = region(p.first, p.second);

        if (r->black()) {
            if (!area) {
                // Keep going. A black region can consume an unknown cell.
            } else if (area->black()) {
                // Keep going. A black region can consume another black region.
            } else { // area->white() || area->numbered()
                continue; // We can't consume this. Discard it.
            }
        } else if (r->white()) {
            if (!area) {
                // Keep going. A white region can consume an unknown cell.
            } else if (area->black()) {
                continue; // We can't consume this. Discard it.
            } else if (area->white()) {
                // Keep going. A white region can consume another white region.
            } else { // area->numbered()
                return false; // Yay! Our region r escaped to a numbered region.
            }
        } else { // r->numbered()
            if (!area) {
                // A numbered region can't consume an unknown cell
                // that's adjacent to another numbered region.
                bool rejected = false;

                for_valid_neighbors(p.first, p.second, [&](const int a, const int b) {
                    const auto& other = region(a, b);

                    if (other && other->numbered() && other != r) {
                        rejected = true;
                    }
                });

                if (rejected) {
                    continue;
                }

                // Keep going. This unknown cell is okay to consume.
            } else if (area->black()) {
                continue; // We can't consume this. Discard it.
            } else if (area->white()) {
                // Keep going. A numbered region can consume a white region.
            } else { // area->numbered()
                throw logic_error("LOGIC ERROR: Grid::confined() - "
                    "I was confused and thought two numbered regions would be adjacent.");
            }
        }

        if (!area) { // Consume an unknown cell.
            flags[p.first + p.second * m_width] = CLOSED;
            ++closed_size;

            for_valid_neighbors(p.first, p.second, [&](const int a, const int b) {
                Flag& f = flags[a + b * m_width];

                if (f == NONE) {
                    f = OPEN;
                }
            });

            if (verboten.empty()) {
                cache[r].insert(p);
            }
        } else { // Consume a whole region.
            for (auto i = area->begin(); i != area->end(); ++i) {
                flags[i->first + i->second * m_width] = CLOSED;
            }

            closed_size += area->size();

            for (auto i = area->unk_begin(); i != area->unk_end(); ++i) {
                Flag& f = flags[i->first + i->second * m_width];

                if (f == NONE) {
                    f = OPEN;
                }
            }
        }
    }

    // We're confined if we still need to consume more cells.
    return (r->black() && closed_size < m_total_black)
        || r->white()
        || (r->numbered() && closed_size < r->number());
}

bool Grid::detect_contradictions(const bool verbose, cache_map_t& cache) {
    auto uh_oh = [&](const string& s) -> bool {
        if (verbose) {
            print(s);
        }

        m_sitrep = CONTRADICTION_FOUND;

        return true;
    };

    for (int x = 0; x < m_width - 1; ++x) {
        for (int y = 0; y < m_height - 1; ++y) {
            if (cell(x, y) == BLACK
                && cell(x + 1, y) == BLACK
                && cell(x, y + 1) == BLACK
                && cell(x + 1, y + 1) == BLACK) {

                return uh_oh("Contradiction found! Pool detected.");
            }
        }
    }

    int black_cells = 0;
    int white_cells = 0;

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        const Region& r = **i;

        // We don't need to look for gigantic black regions because
        // counting black cells is strictly more powerful.

        if ((r.white() && impossibly_big_white_region(r.size()))
            || (r.numbered() && r.size() > r.number())) {

            return uh_oh("Contradiction found! Gigantic region detected.");
        }

        (r.black() ? black_cells : white_cells) += r.size();


        if (confined(*i, cache)) {
            return uh_oh("Contradiction found! Confined region detected.");
        }
    }

    if (black_cells > m_total_black) {
        return uh_oh("Contradiction found! Too many black cells detected.");
    }

    if (white_cells > m_width * m_height - m_total_black) {
        return uh_oh("Contradiction found! Too many white/numbered cells detected.");
    }

    return false;
}

Grid::Grid(const Grid& other)
    : m_width(other.m_width),
    m_height(other.m_height),
    m_total_black(other.m_total_black),
    m_cells(other.m_cells),
    m_regions(),
    m_sitrep(other.m_sitrep),
    m_output(), // Intentionally not copied to increase performance. This copy ctor is private.
    m_prng(other.m_prng) {

    for (auto i = other.m_regions.begin(); i != other.m_regions.end(); ++i) {
        m_regions.insert(make_shared<Region>(**i));
    }

    for (auto i = m_regions.begin(); i != m_regions.end(); ++i) {
        for (auto k = (*i)->begin(); k != (*i)->end(); ++k) {
            region(k->first, k->second) = *i;
        }
    }
}
