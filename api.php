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

// ── Total record count ────────────────────────────────────────────────────────
$countResult = $conn->query("SELECT COUNT(*) AS cnt FROM seismic_data");
if (!$countResult) {
    echo json_encode(["error" => "Count query failed", "message" => $conn->error]);
    exit;
}
$countRow     = $countResult->fetch_assoc();
$totalRecords = (int)($countRow['cnt'] ?? 0);
$totalPages   = max(1, (int)ceil($totalRecords / $perPage));

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

// ── Stats ─────────────────────────────────────────────────────────────────────
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

// ── Active alert — the key fix for 15s frontend display ──────────────────────
// Returns the most recent WARNING or EARTHQUAKE! row whose alert_until
// timestamp is still in the future. The frontend uses this to keep the
// banner/modal alive for the full 15 seconds across every 3s poll cycle.
// If alert_until column does not exist yet, falls back to null gracefully.
$activeAlert = null;
$alertSql = "SELECT status, diff_value,
                    DATE_FORMAT(alert_until, '%Y-%m-%dT%H:%i:%s') AS alert_until
             FROM seismic_data
             WHERE alert_until > NOW()
               AND status IN ('WARNING', 'EARTHQUAKE!')
             ORDER BY recorded_at DESC
             LIMIT 1";

$alertResult = $conn->query($alertSql);
if ($alertResult) {
    $alertRow = $alertResult->fetch_assoc();
    if ($alertRow) {
        $activeAlert = [
            "status"      => $alertRow["status"],
            "diff_value"  => (float)$alertRow["diff_value"],
            "alert_until" => $alertRow["alert_until"],   // ISO string for JS Date()
        ];
    }
}
// If the column doesn't exist the query returns false — $activeAlert stays null.
// No error is thrown so the rest of the response still works.

// ── Response ──────────────────────────────────────────────────────────────────
echo json_encode([
    "records"       => $rows,
    "stats"         => $stats,
    "active_alert"  => $activeAlert,   // null when no active event
    "page"          => $page,
    "total_pages"   => $totalPages,
    "total_records" => $totalRecords,
]);

$conn->close();
?>