import logging

import _stormweaver

logger = logging.getLogger(__name__)


class Workload:
    def __init__(
        self,
        workers,
        duration,
        registry,
        metadata,
        node_factory,
        repeat=1,
        max_reconnect_attempts=5,
        action_config=None,
    ):
        self.num_workers = workers
        self.duration = duration
        self.repeat = repeat
        self.registry = registry
        self.metadata = metadata
        self.node_factory = node_factory
        self.max_reconnect_attempts = max_reconnect_attempts
        self.action_config = action_config
        self._reports = []

    def run(self):
        for cycle in range(self.repeat):
            logger.info("Workload cycle %d/%d", cycle + 1, self.repeat)
            workers = []

            params = _stormweaver.WorkloadParams()
            params.duration_in_seconds = self.duration
            params.max_reconnect_attempts = self.max_reconnect_attempts
            if self.action_config:
                params.action_config = self.action_config

            for i in range(self.num_workers):
                name = f"worker-{cycle + 1}-{i + 1}"
                worker = _stormweaver.RandomWorker(
                    name,
                    self.node_factory,
                    params,
                    self.metadata,
                    self.registry,
                )
                workers.append(worker)

            # run_thread spawns a C++ std::thread internally
            for w in workers:
                w.run_thread(self.duration)

            # join waits for each C++ thread to finish
            for w in workers:
                w.join()

            # Capture reports as strings while workers are still alive
            for w in workers:
                self._reports.append(w.statistics().report())

            logger.info("Workload cycle %d complete", cycle + 1)

    def print_report(self):
        for report in self._reports:
            print(report)
