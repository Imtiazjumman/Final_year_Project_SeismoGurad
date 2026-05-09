<?php
// api.php — SeismoGuard | quakesense database

header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

$host   = "localhost";
$user   = "root";
$pass   = "";
$dbname = "quakesense";

// ── Connect ──────────────────────────────────────────────────────────────────
$conn = new mysqli($host, $user, $pass, $dbname);
if ($conn->connect_error) {
    echo json_encode(["error" => "DB connection failed", "message" => $conn->connect_error]);
    exit;
}

// ── Pagination ───────────────────────────────────────────────────────────────
$perPage = 50;
$page    = isset($_GET['page']) ? max(1, (int)$_GET['page']) : 1;
$offset  = ($page - 1) * $perPage;

// ── Total record count (needed to calculate total pages) ─────────────────────
$countResult = $conn->query("SELECT COUNT(*) AS cnt FROM seismic_data");
if (!$countResult) {
    echo json_encode(["error" => "Count query failed", "message" => $conn->error]);
    exit;
}
$countRow     = $countResult->fetch_assoc();
$totalRecords = (int)($countRow['cnt'] ?? 0);
$totalPages   = max(1, (int)ceil($totalRecords / $perPage));

// Clamp page to valid range
if ($page > $totalPages) $page = $totalPages;
$offset = ($page - 1) * $perPage;

// ── Records for current page ──────────────────────────────────────────────────
$sql    = "SELECT id, status, diff_value,
                  DATE_FORMAT(recorded_at, '%Y-%m-%d %H:%i:%s') AS recorded_at
           FROM seismic_data
           ORDER BY recorded_at DESC
           LIMIT {$perPage} OFFSET {$offset}";

$result = $conn->query($sql);
if (!$result) {
    echo json_encode(["error" => "Records query failed", "message" => $conn->error]);
    exit;
}

$rows = [];
while ($row = $result->fetch_assoc()) {
    $rows[] = $row;
}

// ── Stats (always based on full table, not just current page) ─────────────────
$statsSql = "SELECT
    COUNT(*)                                    AS total,
    IFNULL(SUM(status = 'WARNING'),    0)       AS warnings,
    IFNULL(SUM(status = 'EARTHQUAKE!'), 0)      AS earthquakes,
    IFNULL(MAX(diff_value), 0)                  AS max_diff
FROM seismic_data";

$statsResult = $conn->query($statsSql);
if (!$statsResult) {
    echo json_encode(["error" => "Stats query failed", "message" => $conn->error]);
    exit;
}

$statsRaw = $statsResult->fetch_assoc();
$stats = [
    "total"       => (int)  ($statsRaw["total"]       ?? 0),
    "warnings"    => (int)  ($statsRaw["warnings"]    ?? 0),
    "earthquakes" => (int)  ($statsRaw["earthquakes"] ?? 0),
    "max_diff"    => (float)($statsRaw["max_diff"]    ?? 0),
];

// ── Response ──────────────────────────────────────────────────────────────────
echo json_encode([
    "records"       => $rows,
    "stats"         => $stats,
    "page"          => $page,          // current page number
    "total_pages"   => $totalPages,    // total pages available
    "total_records" => $totalRecords,  // total rows in table
]);

$conn->close();
?>