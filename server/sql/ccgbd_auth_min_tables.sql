-- CCgbd single-schema auth minimum tables/migration
-- Usage:
--   mysql -u <user> -p < server/sql/ccgbd_auth_min_tables.sql

CREATE DATABASE IF NOT EXISTS `CCgbd`
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_general_ci;

-- Auth user table expected by server/src/auth.cpp
CREATE TABLE IF NOT EXISTS `CCgbd`.`users` (
  `id` varchar(50) NOT NULL,
  `password` varchar(50) NOT NULL,
  `name` varchar(20) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- Login audit table expected by server/src/log.cpp
CREATE TABLE IF NOT EXISTS `CCgbd`.`login_logs` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `username` varchar(50) DEFAULT NULL,
  `ip_address` varchar(50) DEFAULT NULL,
  `status` varchar(20) DEFAULT NULL,
  `created_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- One-time data migration from legacy Client_db (if present).
INSERT INTO `CCgbd`.`users` (`id`, `password`, `name`)
SELECT c.`id`, c.`password`, c.`name`
FROM `Client_db`.`users` c
ON DUPLICATE KEY UPDATE
  `password` = VALUES(`password`),
  `name` = VALUES(`name`);

INSERT INTO `CCgbd`.`login_logs` (`username`, `ip_address`, `status`)
SELECT l.`username`, l.`ip_address`, l.`status`
FROM `Client_db`.`login_logs` l
WHERE NOT EXISTS (SELECT 1 FROM `CCgbd`.`login_logs` LIMIT 1);
